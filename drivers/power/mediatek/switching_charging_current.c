#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include <mt-plat/charging.h>
#include <mt-plat/battery_common.h>

#define MODULE_NAME "switching_charging_current"
#define PERIOD_TOTAL_GPU_LOAD (250)
#define PERIOD_TOTAL_CNT      (480)
#define PERIOD_DECREASE       (60)
#define CHECK_STATE_PERIOD    (10)
#define SWITCHING_THRESHOLD   (50)
#define GPU_LOAD_SAMPLING_NUM (30)
#define LIMIT_BACKLIGHT_LEVEL (0)
#define WAIT_ESCAPING_SWITCHING_TIME (30)

extern int get_cur_main_lcd_level(void);
extern bool mtk_get_gpu_loading(unsigned int*);

static void calc_avg_gpu_load(struct work_struct *work);
static struct workqueue_struct *wq_calc_avg_gpu_load;
static DECLARE_DELAYED_WORK(calc_avg_gpu_load_work, calc_avg_gpu_load);

static int current_limit_unlock = true;
static int enable_debugging = true;
static int cnt_state = false;

#define switching_info(fmt, args...) \
	if(enable_debugging) \
		pr_info("[SWITCHING] @@ " fmt, ##args);

enum triggered_reason{
	REASON_NOT_TRIGGERED,
	REASON_CNT,
	REASON_GPU_LOADING
};

enum switching_state{
	STATE_NOT_SWITCHING,
	STATE_WAIT_ESCAPING_SWITCHING,
	STATE_SWITCHING
};

enum clear_level{
	CLEAR_ALL,
	CLEAR_GPU_INFO,
	CLEAR_STATE_INFO,
};

struct _state_info {
	int state;
	int is_triggered;
	int escaping_time;
	unsigned int switching_time;
};

static struct _state_info state_info = {
	.state = STATE_NOT_SWITCHING,
	.is_triggered = REASON_NOT_TRIGGERED,
	.escaping_time = 0,
	.switching_time = 0,
};

struct _gpu_info {
	int gpu_load_sampling[GPU_LOAD_SAMPLING_NUM];
	int sampling_complete;
	int sum_loading;
	int index;
	int avg_loading;
};

static struct _gpu_info gpu_info = {
	.gpu_load_sampling = {0},
	.sampling_complete = false,
	.sum_loading = 0,
	.index = 0,
	.avg_loading = -1,
};

static void clear_info(int lvl){
	switching_info("***** clear informations!! *****\n");

	// clear limit charging current
	current_limit_unlock = true;

	if(lvl == CLEAR_STATE_INFO || lvl == CLEAR_ALL){
		state_info.state = STATE_NOT_SWITCHING;
		state_info.switching_time = 0;
		state_info.escaping_time = 0;
		state_info.is_triggered = REASON_NOT_TRIGGERED;
	}

	if(lvl == CLEAR_GPU_INFO || lvl == CLEAR_ALL){
		for(gpu_info.index = 0; gpu_info.index < GPU_LOAD_SAMPLING_NUM; gpu_info.index++)
			gpu_info.gpu_load_sampling[gpu_info.index] = 0;
		gpu_info.sampling_complete = false;
		gpu_info.index = 0;
		gpu_info.sum_loading = 0;
		gpu_info.avg_loading = -1;
	}
}

static void calc_avg_gpu_load(struct work_struct *work){
	uint32_t gpu_load;

	mtk_get_gpu_loading(&gpu_load);

	//sampling gpu load
	gpu_info.sum_loading -= gpu_info.gpu_load_sampling[gpu_info.index];
	gpu_info.sum_loading += gpu_load;
	gpu_info.gpu_load_sampling[gpu_info.index++] = gpu_load;

	if(gpu_info.sampling_complete){
		gpu_info.avg_loading = (int)(gpu_info.sum_loading / GPU_LOAD_SAMPLING_NUM);
	}else{
		if(gpu_info.index >= GPU_LOAD_SAMPLING_NUM - 1)
			gpu_info.sampling_complete = true;
	}

	gpu_info.index %= GPU_LOAD_SAMPLING_NUM;

	//switching_info("gpu load : %d, sum : %d, avg : %d, idx : %d\n", (int)gpu_load, gpu_info.sum_loading, gpu_info.avg_loading, gpu_info.index);

	if(BMT_status.charger_type == STANDARD_CHARGER){
		queue_delayed_work(wq_calc_avg_gpu_load, &calc_avg_gpu_load_work, msecs_to_jiffies(1000));
	}else{
		clear_info(CLEAR_ALL);
		cancel_delayed_work(&calc_avg_gpu_load_work);
	}
}

static void do_switching(void){
	unsigned int _time;
	
	//switching_info("switching time : %u\n", state_info.switching_time);

	if(state_info.is_triggered == REASON_GPU_LOADING)
		_time = state_info.switching_time % PERIOD_TOTAL_GPU_LOAD;
	else
		_time = state_info.switching_time % PERIOD_TOTAL_CNT;

	if (_time >= PERIOD_DECREASE){
		//switching_info("limit current as 700mA\n");
		current_limit_unlock = true;
	}else{
		//switching_info("limit current as 400mA\n");
		current_limit_unlock = false;
	}
	
	state_info.switching_time += CHECK_STATE_PERIOD;
}

int determine_state(int enabled){
	//switching_info("****** determine state*****\n");

	if(get_cur_main_lcd_level() <= LIMIT_BACKLIGHT_LEVEL || !enabled){
		clear_info(CLEAR_STATE_INFO);
		return current_limit_unlock;
	}
	
	switch(state_info.state){
		case STATE_NOT_SWITCHING:
			switching_info("not scc\n");

			if(cnt_state)
				state_info.is_triggered = REASON_CNT;
			
			if(gpu_info.avg_loading > SWITCHING_THRESHOLD)
				state_info.is_triggered = REASON_GPU_LOADING;

			if(state_info.is_triggered)
				state_info.state = STATE_SWITCHING;

			break;
		case STATE_WAIT_ESCAPING_SWITCHING:
			switching_info("escaping : %d\n", state_info.escaping_time);

			do_switching();

			if(((state_info.is_triggered == REASON_GPU_LOADING) && (gpu_info.avg_loading > SWITCHING_THRESHOLD)) ||
				((state_info.is_triggered == REASON_CNT) && cnt_state)){
				state_info.escaping_time = 0;
				state_info.state = STATE_SWITCHING;
			}

			state_info.escaping_time += CHECK_STATE_PERIOD;

			if(state_info.escaping_time > WAIT_ESCAPING_SWITCHING_TIME)
				clear_info(CLEAR_ALL);	

			if((state_info.is_triggered == REASON_CNT) && (gpu_info.avg_loading > SWITCHING_THRESHOLD))
				clear_info(CLEAR_STATE_INFO);		

			break;
		case STATE_SWITCHING:
			switching_info("scc\n");

			do_switching();

			if(((state_info.is_triggered == REASON_GPU_LOADING) && (gpu_info.avg_loading <= SWITCHING_THRESHOLD)) ||
				((state_info.is_triggered == REASON_CNT) && !cnt_state))
				state_info.state = STATE_WAIT_ESCAPING_SWITCHING;

			if((state_info.is_triggered == REASON_CNT) && (gpu_info.avg_loading > SWITCHING_THRESHOLD))
				clear_info(CLEAR_STATE_INFO);

			break;
		default:
			break;
	}

	return current_limit_unlock;
}


void start_calc_gpu_loading(void){
	calc_avg_gpu_load(0);
}

int dummy_arg;

static int read_condition(char *buffer, const struct kernel_param *kp){
	return sprintf(buffer,"%u %d", BMT_status.charger_type, gpu_info.avg_loading);
}

static int enable_debugging_get(char *buffer, const struct kernel_param *kp){
	return sprintf(buffer,"%d", enable_debugging);
}

static int enable_debugging_set(const char *val, struct kernel_param *kp){	
    if(val[1] == '\n') enable_debugging = val[0] - '0';

	return 0;
}

static int cnt_state_get(char *buffer, const struct kernel_param *kp){
	return sprintf(buffer,"%d", cnt_state);
}

static int cnt_state_set(const char *val, struct kernel_param *kp){	
    if(val[1] == '\n') cnt_state = val[0] - '0';

	return 0;
}

module_param_call(condition, NULL, read_condition, &dummy_arg, S_IWUSR | S_IRUGO);
module_param_call(debugging, enable_debugging_set, enable_debugging_get, &dummy_arg, S_IWUSR | S_IRUGO);
module_param_call(cnt_state, cnt_state_set, cnt_state_get, &dummy_arg, S_IWUSR | S_IRUGO);


static int lge_switching_charging_current_probe(struct platform_device *pdev)
{
	int ret = 0;

	wq_calc_avg_gpu_load = create_singlethread_workqueue("wq_calc_avg_gpu_load");

	queue_delayed_work(wq_calc_avg_gpu_load, &calc_avg_gpu_load_work, msecs_to_jiffies(30000));

	pr_info("[SWITCHING] probe done!!\n");

	return ret;
}

static int lge_switching_charging_current_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver lge_switching_charging_current_driver = {
	.probe = lge_switching_charging_current_probe,
	.remove = lge_switching_charging_current_remove,
	.driver = {
		.name = MODULE_NAME,
		.owner = THIS_MODULE,
	},
};

static struct platform_device lge_switching_charging_current_device = {
	.name = MODULE_NAME,
	.dev = {
		.platform_data = NULL,
	}
};

static int __init lge_switching_charging_current_init(void)
{
	platform_device_register(&lge_switching_charging_current_device);

	return platform_driver_register(&lge_switching_charging_current_driver);
}

static void __exit lge_switching_charging_current_exit(void)
{
	platform_driver_unregister(&lge_switching_charging_current_driver);
}

module_init(lge_switching_charging_current_init);
module_exit(lge_switching_charging_current_exit);

MODULE_DESCRIPTION("LGE switching_charging_current");
MODULE_LICENSE("GPL");
