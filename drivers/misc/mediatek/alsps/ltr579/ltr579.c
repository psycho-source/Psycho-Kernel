/*
 * 
 * Author: MingHsien Hsieh <minghsien.hsieh@mediatek.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <linux/earlysuspend.h>
#include <linux/platform_device.h>
#include <asm/atomic.h>
#include <mach/mt_typedefs.h>
#include <mach/mt_gpio.h>
#include <mach/mt_pm_ldo.h>
#include <linux/hwmsensor.h>
#include <linux/hwmsen_dev.h>
#include <linux/sensors_io.h>
#include <asm/io.h>
#include <cust_eint.h>
#include <cust_alsps.h>
#include <linux/hwmsen_helper.h>
#include "ltr579.h"
#include "linux/mutex.h"
#include <mach/mt_boot.h>
#include <alsps.h>

#define GN_MTK_BSP_PS_DYNAMIC_CALI
#define GN_MTK_BSP_PS_DYNAMIC_CALI_NUM 2
#define POWER_NONE_MACRO MT65XX_POWER_NONE
#define MAX_ELM 30
/******************************************************************************
 * configuration
*******************************************************************************/
/*----------------------------------------------------------------------------*/
static int log_out_enable = 0;

#define LTR579_DEV_NAME   "LTR_579ALS"

/*----------------------------------------------------------------------------*/
#define APS_TAG                  "[ALS/PS] "
#define APS_FUN(f) \
	do{\
	        if(1)\
		        printk(KERN_ERR APS_TAG"%s\n", __FUNCTION__);}\
	while(0)

#define APS_ERR(fmt, args...) \
	do{\
	        if(1)\
		        printk(KERN_ERR  APS_TAG"%s %d : "fmt, __FUNCTION__, __LINE__, ##args);}\
	while(0)
		
#define APS_LOG(fmt, args...) \
	do{\
	        if(log_out_enable != 0)\
		        printk(KERN_ERR APS_TAG fmt, ##args);}\
	while(0)

#define APS_DBG(fmt, args...) \
	do{\
	        if(log_out_enable != 0)\
		        printk(KERN_ERR APS_TAG fmt, ##args);}\
	while(0)
		
/******************************************************************************
 * extern functions
*******************************************************************************/

extern void mt_eint_mask(unsigned int eint_num);
extern void mt_eint_unmask(unsigned int eint_num);
extern void mt_eint_set_hw_debounce(unsigned int eint_num, unsigned int ms);
extern void mt_eint_set_polarity(unsigned int eint_num, unsigned int pol);
extern unsigned int mt_eint_set_sens(unsigned int eint_num, unsigned int sens);
extern void mt_eint_registration(unsigned int eint_num, unsigned int flow, void (EINT_FUNC_PTR)(void), unsigned int is_auto_umask);
extern void mt_eint_print_status(void);
/*----------------------------------------------------------------------------*/
static int  ltr579_local_init(void);
static int ltr579_remove(struct platform_device *pdev);
static struct alsps_init_info ltr579_init_info = {
		.name = "ltr579",
		.init = ltr579_local_init,
		.uninit = ltr579_remove,
};

static struct i2c_client *ltr579_i2c_client = NULL;
static int isadjust=0;
static int dynamic_cali = 0;
static int als_test2real_rate1=21;
static int als_test2real_rate2=16;

static unsigned short record[MAX_ELM];
static int rct=0,full=0;
static long lux_sum=0;

static void ps_detect_work_func(struct work_struct *work);
static DECLARE_DELAYED_WORK(ps_detect_work, ps_detect_work_func);
static unsigned long poll_delay = 25;
static int is_timer_active = 0;

typedef struct{
    unsigned  int result_ps_cali_value;
    unsigned  int result_als_cali_value;
}LTR579_THRE;
unsigned int als_cali_multiply_num = 1000;
/*----------------------------------------------------------------------------*/
static const struct i2c_device_id ltr579_i2c_id[] = {{LTR579_DEV_NAME,0},{}};
/*the adapter id & i2c address will be available in customization*/
static struct i2c_board_info __initdata i2c_ltr579={ I2C_BOARD_INFO("LTR_579ALS", 0x53)};

//static unsigned short ltr579_force[] = {0x00, 0x46, I2C_CLIENT_END, I2C_CLIENT_END};
//static const unsigned short *const ltr579_forces[] = { ltr579_force, NULL };
//static struct i2c_client_address_data ltr579_addr_data = { .forces = ltr579_forces,};
/*----------------------------------------------------------------------------*/
static int ltr579_init_flag =-1; // 0<==>OK -1 <==> fail
static int ltr579_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id); 
static int ltr579_i2c_remove(struct i2c_client *client);
static int ltr579_i2c_detect(struct i2c_client *client, int kind, struct i2c_board_info *info);
/*----------------------------------------------------------------------------*/
static int ltr579_i2c_suspend(struct i2c_client *client, pm_message_t msg);
static int ltr579_i2c_resume(struct i2c_client *client);
static int ltr579_ps_enable();
static int ltr579_ps_disable();
static int ltr579_als_disable();
static int ltr579_als_enable(int gainrange);
	
static int als_gainrange;

static int final_prox_val;
static int final_lux_val;

static int prox_thresh_low = 0;
static int prox_thresh_high = 0;

/*----------------------------------------------------------------------------*/
static DEFINE_MUTEX(read_lock);

/*----------------------------------------------------------------------------*/
static int ltr579_als_read(int gainrange);
static int ltr579_ps_read(void);


/*----------------------------------------------------------------------------*/


typedef enum {
    CMC_BIT_ALS    = 1,
    CMC_BIT_PS     = 2,
} CMC_BIT;

/*----------------------------------------------------------------------------*/
struct ltr579_i2c_addr {    /*define a series of i2c slave address*/
    u8  write_addr;  
    u8  ps_thd;     /*PS INT threshold*/
};

/*----------------------------------------------------------------------------*/

struct ltr579_priv {
    struct alsps_hw  *hw;
    struct i2c_client *client;
    struct work_struct  eint_work;
    struct mutex lock;
	/*i2c address group*/
    struct ltr579_i2c_addr  addr;

     /*misc*/
    u16		    als_modulus;
    atomic_t    i2c_retry;
    atomic_t    als_debounce;   /*debounce time after enabling als*/
    atomic_t    als_deb_on;     /*indicates if the debounce is on*/
    atomic_t    als_deb_end;    /*the jiffies representing the end of debounce*/
    atomic_t    ps_mask;        /*mask ps: always return far away*/
    atomic_t    ps_debounce;    /*debounce time after enabling ps*/
    atomic_t    ps_deb_on;      /*indicates if the debounce is on*/
    atomic_t    ps_deb_end;     /*the jiffies representing the end of debounce*/
    atomic_t    ps_suspend;
    atomic_t    als_suspend;

    /*data*/
    u16         als;
    u16          ps;
    u8          _align;
    u16         als_level_num;
    u16         als_value_num;
    u32         als_level[C_CUST_ALS_LEVEL-1];
    u32         als_value[C_CUST_ALS_LEVEL];

    atomic_t    als_cmd_val;    /*the cmd value can't be read, stored in ram*/
    atomic_t    ps_cmd_val;     /*the cmd value can't be read, stored in ram*/
    atomic_t    ps_thd_val;     /*the cmd value can't be read, stored in ram*/
	atomic_t    ps_thd_val_high;     /*the cmd value can't be read, stored in ram*/
	atomic_t    ps_thd_val_low;     /*the cmd value can't be read, stored in ram*/
    ulong       enable;         /*enable mask*/
    ulong       pending_intr;   /*pending interrupt*/

    /*early suspend*/
#if defined(CONFIG_HAS_EARLYSUSPEND)
    struct early_suspend    early_drv;
#endif     
};

 struct PS_CALI_DATA_STRUCT
{
    int close;
    int far_away;
    int valid;
} ;

static struct PS_CALI_DATA_STRUCT ps_cali={0,0,0};
static int intr_flag_value = 0;


static struct ltr579_priv *ltr579_obj = NULL;
static struct platform_driver ltr579_alsps_driver;

/*----------------------------------------------------------------------------*/
static struct i2c_driver ltr579_i2c_driver = {	
	.probe      = ltr579_i2c_probe,
	.remove     = ltr579_i2c_remove,
	.detect     = ltr579_i2c_detect,
	//.suspend    = ltr579_i2c_suspend,
	//.resume     = ltr579_i2c_resume,
	.id_table   = ltr579_i2c_id,
	//.address_data = &ltr579_addr_data,
	.driver = {
		//.owner          = THIS_MODULE,
		.name           = LTR579_DEV_NAME,
	},
};


/* 
 * #########
 * ## I2C ##
 * #########
 */

// I2C Read
static int ltr579_i2c_read_reg(u8 regnum)
{
    u8 buffer[1],reg_value[1];
	int res = 0;
	mutex_lock(&read_lock);
	
	buffer[0]= regnum;
	res = i2c_master_send(ltr579_obj->client, buffer, 0x1);
	if(res <= 0)	{
		mutex_unlock(&read_lock);
		APS_ERR("read reg send res = %d\n",res);
		return res;
	}
	res = i2c_master_recv(ltr579_obj->client, reg_value, 0x1);
	if(res <= 0)
	{
		mutex_unlock(&read_lock);
		APS_ERR("read reg recv res = %d\n",res);
		return res;
	}
	mutex_unlock(&read_lock);
	return reg_value[0];
}

// I2C Write
static int ltr579_i2c_write_reg(u8 regnum, u8 value)
{
	u8 databuf[2];    
	int res = 0;
   
	databuf[0] = regnum;   
	databuf[1] = value;
	res = i2c_master_send(ltr579_obj->client, databuf, 0x2);

	if (res < 0)
		{
			APS_ERR("wirte reg send res = %d\n",res);
		   	return res;
		}
		
	else
		return 0;
}


/*----------------------------------------------------------------------------*/
static ssize_t ltr579_show_als(struct device_driver *ddri, char *buf)
{
	int res;
	u8 dat = 0;
	
	if(!ltr579_obj)
	{
		APS_ERR("ltr579_obj is null!!\n");
		return 0;
	}
	res = ltr579_als_read(als_gainrange);
    return snprintf(buf, PAGE_SIZE, "0x%04X\n", ((res*als_test2real_rate1)/als_test2real_rate2));
	
}
/*----------------------------------------------------------------------------*/
static ssize_t ltr579_show_ps(struct device_driver *ddri, char *buf)
{
	int  res;
	if(!ltr579_obj)
	{
		APS_ERR("ltr579_obj is null!!\n");
		return 0;
	}
	res = ltr579_ps_read();
    return snprintf(buf, PAGE_SIZE, "0x%04X\n", res);     
}
/*----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------*/
static ssize_t ltr579_show_status(struct device_driver *ddri, char *buf)
{
	ssize_t len = 0;
	
	if(!ltr579_obj)
	{
		APS_ERR("ltr579_obj is null!!\n");
		return 0;
	}
	
	if(ltr579_obj->hw)
	{
	
		len += snprintf(buf+len, PAGE_SIZE-len, "CUST: %d, (%d %d)\n", 
			ltr579_obj->hw->i2c_num, ltr579_obj->hw->power_id, ltr579_obj->hw->power_vol);
		
	}
	else
	{
		len += snprintf(buf+len, PAGE_SIZE-len, "CUST: NULL\n");
	}


	len += snprintf(buf+len, PAGE_SIZE-len, "MISC: %d %d\n", atomic_read(&ltr579_obj->als_suspend), atomic_read(&ltr579_obj->ps_suspend));

	return len;
}

/*----------------------------------------------------------------------------*/
static ssize_t ltr579_store_status(struct device_driver *ddri, char *buf, size_t count)
{
	int status1,ret;
	if(!ltr579_obj)
	{
		APS_ERR("ltr579_obj is null!!\n");
		return -EINVAL;
	}
	
	if(1 == sscanf(buf, "%d ", &status1))
	{ 
	   if(status1==1)
	   	{
		    ret=ltr579_ps_enable();
	   		if (ret < 0)
				APS_DBG("iret= %d \n", ret);
	   	}
	   else if(status1==2)
	   	{
			ret = ltr579_als_enable(als_gainrange);
			if (ret < 0)
				APS_DBG("iret= %d \n", ret);
	   	}
	   else if(status1==0)
	   	{
			ret=ltr579_ps_disable();
	   		if (ret < 0)
				APS_DBG("iret= %d \n", ret);
			
			ret=ltr579_als_disable();
	   		if (ret < 0)
				APS_DBG("iret= %d \n", ret);
	   	}
	}
	else
	{
		//APS_DBG("invalid content: '%s', length = %d\n", buf, count);
		APS_DBG("invalid content: '%s'\n", buf);
	}
	
	return count;    
}


static ssize_t ltr579_log_flag_store(struct device_driver *ddri, char *buf, size_t count)
{
	int status1,ret;
	if(!ltr579_obj)
	{
		APS_ERR("ltr579_obj is null!!\n");
		return -EINVAL;
	}
	
	if(1 == sscanf(buf, "%d ", &status1))
	{ 
	   if(status1 > 0)
	   {
			log_out_enable=1;
	   }
	   else
	{
			log_out_enable = 0;
	   }
		
	}
	
	return count;    
}

/*----------------------------------------------------------------------------*/
static ssize_t ltr579_show_reg(struct device_driver *ddri, char *buf, size_t count)
{
	int i,len=0;
	int reg[]={0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0d,0x0e,0x0f,
				0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26};
	for(i=0;i<27;i++)
		{
		len += snprintf(buf+len, PAGE_SIZE-len, "reg:0x%04X value: 0x%04X\n", reg[i],ltr579_i2c_read_reg(reg[i]));	

	    }
	return len;
}
/*----------------------------------------------------------------------------*/
static ssize_t ltr579_show_intr(struct device_driver *ddri, char *buf, size_t count)
{
	int  res;
	if(!ltr579_obj)
	{
		APS_ERR("ltr579_obj is null!!\n");
		return 0;
	}
	res = intr_flag_value;
    return snprintf(buf, PAGE_SIZE, "0x%04X\n", res);

}
/*----------------------------------------------------------------------------*/
static ssize_t ltr579_show_threshold(struct device_driver *ddri, char *buf, size_t count)
{
	int  res;
	if(!ltr579_obj)
	{
		APS_ERR("ltr579_obj is null!!\n");
		return 0;
	}
	res = intr_flag_value;
    return snprintf(buf, PAGE_SIZE, "%d, %d\n", atomic_read(&ltr579_obj->ps_thd_val_low), atomic_read(&ltr579_obj->ps_thd_val_high));

}

/*----------------------------------------------------------------------------*/
static ssize_t ltr579_store_reg(struct device_driver *ddri, char *buf, size_t count)
{
	int ret,value;
	unsigned int  reg;
	if(!ltr579_obj)
	{
		APS_ERR("ltr579_obj is null!!\n");
		return -EINVAL;
	}
	
	if(2 == sscanf(buf, "%x %x ", &reg,&value))
	{ 
		APS_DBG("before write reg: %x, reg_value = %x  write value=%x\n", reg,ltr579_i2c_read_reg(reg),value);
	    ret=ltr579_i2c_write_reg(reg,value);
		APS_DBG("after write reg: %x, reg_value = %x\n", reg,ltr579_i2c_read_reg(reg));
	}
	else
	{
		//APS_DBG("invalid content: '%s', length = %d\n", buf, count); //sang
		APS_DBG("invalid content: '%s'\n", buf);
	}
	return count;    
}

/*----------------------------------------------------------------------------*/
static DRIVER_ATTR(als,     S_IWUSR | S_IRUGO, ltr579_show_als,   NULL);
static DRIVER_ATTR(ps,      S_IWUSR | S_IRUGO, ltr579_show_ps,    NULL);
//static DRIVER_ATTR(config,  S_IWUSR | S_IRUGO, ltr579_show_config,ltr579_store_config);
//static DRIVER_ATTR(alslv,   S_IWUSR | S_IRUGO, ltr579_show_alslv, ltr579_store_alslv);
//static DRIVER_ATTR(alsval,  S_IWUSR | S_IRUGO, ltr579_show_alsval,ltr579_store_alsval);
//static DRIVER_ATTR(trace,   S_IWUSR | S_IRUGO,ltr579_show_trace, ltr579_store_trace);
static DRIVER_ATTR(status,  S_IWUSR | S_IRUGO, ltr579_show_status,  ltr579_store_status);
static DRIVER_ATTR(reg,     S_IWUSR | S_IRUGO, ltr579_show_reg,   ltr579_store_reg);
static DRIVER_ATTR(intr,     S_IWUSR | S_IRUGO, ltr579_show_intr,   NULL);
static DRIVER_ATTR(threshold,     S_IWUSR | S_IRUGO, ltr579_show_threshold,   NULL);
static DRIVER_ATTR(log_flag_enable,     S_IWUSR | S_IRUGO, NULL,   ltr579_log_flag_store);


//static DRIVER_ATTR(i2c,     S_IWUSR | S_IRUGO, ltr579_show_i2c,   ltr579_store_i2c);
/*----------------------------------------------------------------------------*/
static struct driver_attribute *ltr579_attr_list[] = {
    &driver_attr_als,
    &driver_attr_ps,    
   // &driver_attr_trace,        /*trace log*/
   // &driver_attr_config,
   // &driver_attr_alslv,
   // &driver_attr_alsval,
    &driver_attr_status,
   //&driver_attr_i2c,
   &driver_attr_log_flag_enable,
    &driver_attr_reg,
    &driver_attr_intr,
    &driver_attr_threshold,
};

/*----------------------------------------------------------------------------*/
static int ltr579_create_attr(struct driver_attribute *driver) 
{
	int idx, err = 0;
	int num = (int)(sizeof(ltr579_attr_list)/sizeof(ltr579_attr_list[0]));

	if (driver == NULL)
	{
		return -EINVAL;
	}

	for(idx = 0; idx < num; idx++)
	{
		if(err = driver_create_file(driver, ltr579_attr_list[idx]))
		{            
			APS_ERR("driver_create_file (%s) = %d\n", ltr579_attr_list[idx]->attr.name, err);
			break;
		}
	}    
	return err;
}
/*----------------------------------------------------------------------------*/
	static int ltr579_delete_attr(struct device_driver *driver)
	{
	int idx ,err = 0;
	int num = (int)(sizeof(ltr579_attr_list)/sizeof(ltr579_attr_list[0]));

	if (!driver)
	return -EINVAL;

	for (idx = 0; idx < num; idx++) 
	{
		driver_remove_file(driver, ltr579_attr_list[idx]);
	}
	
	return err;
}

/*----------------------------------------------------------------------------*/

/* 
 * ###############
 * ## PS CONFIG ##
 * ###############

 */
#ifdef GN_MTK_BSP_PS_DYNAMIC_CALI
static int ltr579_dynamic_calibrate(void)
{
	int ret = 0;
	int i = 0;
	int j = 0;
	int data = 0;
	int noise = 0;
	int len = 0;
	int err = 0;
	int max = 0;
	int idx_table = 0;
	unsigned long data_total = 0;
	struct ltr579_priv *obj = ltr579_obj;
	int ps_thd_val_low, ps_thd_val_high;


	APS_FUN(f);
	if (!obj) goto err;

	mdelay(10);
	for (i = 0; i < GN_MTK_BSP_PS_DYNAMIC_CALI_NUM; i++) {
		if (max++ > 5) {
			atomic_set(&obj->ps_thd_val_high,  2047);
			atomic_set(&obj->ps_thd_val_low, 2047);

			goto err;
		}
		mdelay(50);
		
		data = ltr579_ps_read();
		if(data == 0){
			j++;
		}	
		data_total += data;
	}
	noise = data_total/(GN_MTK_BSP_PS_DYNAMIC_CALI_NUM - j);
	dynamic_cali = noise;
	isadjust = 1;
 
	if(noise < 300){
#if 0		
			atomic_set(&obj->ps_thd_val_high,  400);
			atomic_set(&obj->ps_thd_val_low, 360);
#else
			atomic_set(&obj->ps_thd_val_high,  noise+190);
			atomic_set(&obj->ps_thd_val_low, noise+100);
#endif
	}else if(noise < 400){
			atomic_set(&obj->ps_thd_val_high,  noise+230);
			atomic_set(&obj->ps_thd_val_low, noise+100);
	}else if(noise < 600){
			atomic_set(&obj->ps_thd_val_high,  noise+250);
			atomic_set(&obj->ps_thd_val_low, noise+120);
	}else if(noise < 1000){
			atomic_set(&obj->ps_thd_val_high,  noise+260);
			atomic_set(&obj->ps_thd_val_low, noise+140);
	}else if(noise < 1450){
			atomic_set(&obj->ps_thd_val_high,  noise+500);
			atomic_set(&obj->ps_thd_val_low, noise+300);
	}
	else{
			atomic_set(&obj->ps_thd_val_high,  1600);
			atomic_set(&obj->ps_thd_val_low, 1450);
			isadjust = 0;
	}
	ps_thd_val_low = atomic_read(&obj->ps_thd_val_low);
	ps_thd_val_high = atomic_read(&obj->ps_thd_val_high);

	return 0;
err:
	APS_ERR("ltr579_dynamic_calibrate fail!!!\n");
	return -1;
}
#endif

static int ltr579_ps_set_thres()
{
	APS_FUN();

	int res;
	u8 databuf[2];
	
		struct i2c_client *client = ltr579_obj->client;
		struct ltr579_priv *obj = ltr579_obj;
		APS_DBG("ps_cali.valid: %d\n", ps_cali.valid);
	if(1 == ps_cali.valid)
	{
		databuf[0] = LTR579_PS_THRES_LOW_0; 
		databuf[1] = (u8)(ps_cali.far_away & 0x00FF);
		res = i2c_master_send(client, databuf, 0x2);
		if(res <= 0)
		{
			goto EXIT_ERR;
			return LTR579_ERR_I2C;
		}
		databuf[0] = LTR579_PS_THRES_LOW_1; 
		databuf[1] = (u8)((ps_cali.far_away & 0xFF00) >> 8);
		res = i2c_master_send(client, databuf, 0x2);
		if(res <= 0)
		{
			goto EXIT_ERR;
			return LTR579_ERR_I2C;
		}
		databuf[0] = LTR579_PS_THRES_UP_0;	
		databuf[1] = (u8)(ps_cali.close & 0x00FF);
		res = i2c_master_send(client, databuf, 0x2);
		if(res <= 0)
		{
			goto EXIT_ERR;
			return LTR579_ERR_I2C;
		}
		databuf[0] = LTR579_PS_THRES_UP_1;	
		databuf[1] = (u8)((ps_cali.close & 0xFF00) >> 8);;
		res = i2c_master_send(client, databuf, 0x2);
		if(res <= 0)
		{
			goto EXIT_ERR;
			return LTR579_ERR_I2C;
		}
		prox_thresh_low = ps_cali.far_away;
		prox_thresh_high = ps_cali.close;
	}
	else
	{
		databuf[0] = LTR579_PS_THRES_LOW_0; 
		databuf[1] = (u8)((atomic_read(&obj->ps_thd_val_low)) & 0x00FF);
		res = i2c_master_send(client, databuf, 0x2);
		if(res <= 0)
		{
			goto EXIT_ERR;
			return LTR579_ERR_I2C;
		}
		databuf[0] = LTR579_PS_THRES_LOW_1; 
		databuf[1] = (u8)((atomic_read(&obj->ps_thd_val_low )>> 8) & 0x00FF);
		
		res = i2c_master_send(client, databuf, 0x2);
		if(res <= 0)
		{
			goto EXIT_ERR;
			return LTR579_ERR_I2C;
		}
		databuf[0] = LTR579_PS_THRES_UP_0;	
		databuf[1] = (u8)((atomic_read(&obj->ps_thd_val_high)) & 0x00FF);
		res = i2c_master_send(client, databuf, 0x2);
		if(res <= 0)
		{
			goto EXIT_ERR;
			return LTR579_ERR_I2C;
		}
		databuf[0] = LTR579_PS_THRES_UP_1;	
		databuf[1] = (u8)((atomic_read(&obj->ps_thd_val_high) >> 8) & 0x00FF);
		res = i2c_master_send(client, databuf, 0x2);
		if(res <= 0)
		{
			goto EXIT_ERR;
			return LTR579_ERR_I2C;
		}
		prox_thresh_low = atomic_read(&obj->ps_thd_val_low);
		prox_thresh_high = atomic_read(&obj->ps_thd_val_high);
	}
	APS_LOG("ltr579_ps_set_thres : low_thresh=%d, high_thresh=%d.\n", prox_thresh_low, prox_thresh_high);

	res = 0;
	return res;
	
	EXIT_ERR:
	APS_ERR("set thres: %d\n", res);
	return res;

}


static int ltr579_ps_enable()
{
	struct i2c_client *client = ltr579_obj->client;
	struct ltr579_priv *obj = ltr579_obj;
	u8 databuf[2];	
	int res;

	int error;
	int setctrl;
	int ps_raw_data = 0;
	int ps_val = 0;
	hwm_sensor_data sensor_data;
    APS_LOG("ltr579_ps_enable() ...start!\n");

	
	mdelay(WAKEUP_DELAY);
    
	/* =============== 
	 * ** IMPORTANT **
	 * ===============
	 * Other settings like timing and threshold to be set here, if required.
 	 * Not set and kept as device default for now.
 	 */
   	error = ltr579_i2c_write_reg(LTR579_PS_PULSES, 32); //32pulses 
	if(error<0)
    {
        APS_LOG("ltr579_ps_enable() PS Pulses error2\n");
	    return error;
	} 
	error = ltr579_i2c_write_reg(LTR579_PS_LED, 0x36); // 60khz & 100mA 
	if(error<0)
    {
        APS_LOG("ltr579_ps_enable() PS LED error...\n");
	    return error;
	}
		error = ltr579_i2c_write_reg(LTR579_PS_MEAS_RATE, 0x5C); // 11bits & 50ms time 
	if(error<0)
    {
        APS_LOG("ltr579_ps_enable() PS time error...\n");
	    return error;
	}


	/*for interrup work mode support -- by WeeLiat, Liteon 18.06.2015*/
		if(0 == obj->hw->polling_mode_ps)
		{		

			ltr579_ps_set_thres();
			
			databuf[0] = LTR579_INT_CFG;	
			databuf[1] = 0x01;
			res = i2c_master_send(client, databuf, 0x2);
			if(res <= 0)
			{
				goto EXIT_ERR;
				return LTR579_ERR_I2C;
			}
	
			databuf[0] = LTR579_INT_PST;	
			databuf[1] = 0x02;
			res = i2c_master_send(client, databuf, 0x2);
			if(res <= 0)
			{
				goto EXIT_ERR;
				return LTR579_ERR_I2C;
			}
			mt_eint_unmask(CUST_EINT_ALS_NUM);
	
		}
	
 	setctrl = ltr579_i2c_read_reg(LTR579_MAIN_CTRL);
	if((setctrl & 0x01) == 0)//Check for PS enable?
	{
		setctrl = setctrl | 0x01;
		error = ltr579_i2c_write_reg(LTR579_MAIN_CTRL, setctrl); 
		if(error<0)
		{
	    APS_LOG("ltr579_ps_enable() error1\n");
	    return error;
		}
	}
	
	APS_LOG("ltr579_ps_enable ...OK!\n");
	
	#ifdef GN_MTK_BSP_PS_DYNAMIC_CALI
	ltr579_dynamic_calibrate();
	#endif
	ltr579_ps_set_thres();

	//schedule_work(&obj->eint_work);
	mdelay(50);
	ps_raw_data = ltr579_ps_read();
	if(ps_raw_data > prox_thresh_high)    //near
	{
		ps_val = 0;
		intr_flag_value = 1;
	}
	else                                  //far
	{
		ps_val = 5;
		intr_flag_value =0;
	}
	ps_report_interrupt_data(ps_val);
	return error;

	EXIT_ERR:
	APS_ERR("set thres: %d\n", res);
	return res;
}

// Put PS into Standby mode
static int ltr579_ps_disable(void)
{
	int error;
	struct ltr579_priv *obj = ltr579_obj;
	int setctrl;

	if(1 == is_timer_active)
	{
		cancel_delayed_work_sync(&ps_detect_work);
		is_timer_active = 0;
	}
	
	setctrl = ltr579_i2c_read_reg(LTR579_MAIN_CTRL);
	if(setctrl & 0x01 == 1)
	{
		setctrl = setctrl & 0xFE; 	
	}
	
	error = ltr579_i2c_write_reg(LTR579_MAIN_CTRL, setctrl);  //sang
	if(error<0)
	    APS_LOG("ltr579_ps_disable ...ERROR\n");
 	else
        APS_LOG("ltr579_ps_disable ...OK\n");

	if(0 == obj->hw->polling_mode_ps)
	{
	    cancel_work_sync(&obj->eint_work);
		mt_eint_mask(CUST_EINT_ALS_NUM);
	}
	
	return error;
}


static int ltr579_ps_read(void)
{
	int psval_lo, psval_hi, psdata;

	psval_lo = ltr579_i2c_read_reg(LTR579_PS_DATA_0);
	APS_DBG("ps_rawdata_psval_lo = %d\n", psval_lo);
	if (psval_lo < 0){
	    
	    APS_DBG("psval_lo error\n");
		psdata = psval_lo;
		goto out;
	}
	psval_hi = ltr579_i2c_read_reg(LTR579_PS_DATA_1);
    APS_DBG("ps_rawdata_psval_hi = %d\n", psval_hi);

	if (psval_hi < 0){
	    APS_DBG("psval_hi error\n");
		psdata = psval_hi;
		goto out;
	}
	
	psdata = ((psval_hi & 7)* 256) + psval_lo;
    //psdata = ((psval_hi&0x7)<<8) + psval_lo;
    APS_DBG("ps_rawdata = %d\n", psdata);
    
	out:
	final_prox_val = psdata;
	
	return psdata;
}

/* 
 * ################
 * ## ALS CONFIG ##
 * ################
 */

static int ltr579_als_enable(int gainrange)
{
	int error;
	int setctrl;
	int is_ps_active = 0;
	APS_LOG("gainrange = %d\n",gainrange);
	switch (gainrange)
	{
		case ALS_RANGE_1:
			error = ltr579_i2c_write_reg(LTR579_ALS_GAIN, MODE_ALS_Range1);
			break;

		case ALS_RANGE_3:
			error = ltr579_i2c_write_reg(LTR579_ALS_GAIN, MODE_ALS_Range3);
			break;

		case ALS_RANGE_6:
			error = ltr579_i2c_write_reg(LTR579_ALS_GAIN, MODE_ALS_Range6);
			break;
			
		case ALS_RANGE_9:
			error = ltr579_i2c_write_reg(LTR579_ALS_GAIN, MODE_ALS_Range9);
			break;
			
		case ALS_RANGE_18:
			error = ltr579_i2c_write_reg(LTR579_ALS_GAIN, MODE_ALS_Range18);
			break;
		
		default:
			error = ltr579_i2c_write_reg(LTR579_ALS_GAIN, MODE_ALS_Range3);			
			APS_ERR("ALS sensor gainrange %d!\n", gainrange);
			break;
	}

	error = ltr579_i2c_write_reg(LTR579_ALS_MEAS_RATE, ALS_RESO_MEAS);// 18 bit & 100ms measurement rate		
	APS_LOG("ALS sensor resolution & measurement rate: %d!\n", ALS_RESO_MEAS );	

	setctrl = ltr579_i2c_read_reg(LTR579_MAIN_CTRL);
	setctrl = setctrl | 0x02;// Enable ALS

	if((setctrl & 0x01) == 1)
		is_ps_active = 1;
	
	error = ltr579_i2c_write_reg(LTR579_MAIN_CTRL, setctrl);	

	mdelay(WAKEUP_DELAY);

	if( 1 == is_ps_active && 1 == intr_flag_value)    //ps is active and is near irq now.
	{
		schedule_work(&ps_detect_work);
		is_timer_active = 1;
	}

	/* =============== 
	 * ** IMPORTANT **
	 * ===============
	 * Other settings like timing and threshold to be set here, if required.
 	 * Not set and kept as device default for now.
 	 */
 	if(error<0)
 	    APS_LOG("ltr579_als_enable ...ERROR\n");
 	else
        APS_LOG("ltr579_als_enable ...OK\n");
        
	return error;
}


// Put ALS into Standby mode
static int ltr579_als_disable(void)
{
	int error;
	int setctrl;
	int is_ps_active = 0;
	
	setctrl = ltr579_i2c_read_reg(LTR579_MAIN_CTRL);
	setctrl = setctrl & 0xFD;// disable ALS

	if((setctrl & 0x01) == 1)
		is_ps_active = 1;

	error = ltr579_i2c_write_reg(LTR579_MAIN_CTRL, setctrl); 
	if(error<0)
 	    APS_LOG("ltr579_als_disable ...ERROR\n");
 	else
        APS_LOG("ltr579_als_disable ...OK\n");

	if( 1 == is_ps_active && 1 == intr_flag_value)    //ps is active and is near irq now.
	{
		schedule_work(&ps_detect_work);
		is_timer_active = 1;
	}


	return error;
}

static int ltr579_als_read(int gainrange)
{
	unsigned int alsval_0, alsval_1, alsval_2, alsval;
	int luxdata_int;
    int winfac=13;// default value, while recommed value is 8
	
	alsval_0 = ltr579_i2c_read_reg(LTR579_ALS_DATA_0);
	alsval_1 = ltr579_i2c_read_reg(LTR579_ALS_DATA_1);
	alsval_2 = ltr579_i2c_read_reg(LTR579_ALS_DATA_2);
	alsval = (alsval_2 * 256* 256) + (alsval_1 * 256) + alsval_0;
	APS_DBG("alsval_0 = %d,alsval_1=%d,alsval_2=%d,alsval=%d\n",alsval_0,alsval_1,alsval_2,alsval);

    if(alsval==0)
    {
        luxdata_int = 0;
        goto err;
    }

	APS_DBG("gainrange = %d\n",gainrange);

	luxdata_int = alsval*8/gainrange;//formula: ALS counts * 0.8/gain/int , int=1
	luxdata_int = luxdata_int*winfac/10;
	APS_DBG("Amin:als_value_lux = %d\n", luxdata_int);
#if 1    //add amin
    luxdata_int *= als_cali_multiply_num;
    luxdata_int /= 1000;
#endif 
	APS_DBG("Alvin:als_value_lux =%d multiply=%d\n", luxdata_int,als_cali_multiply_num);
	return luxdata_int;

	
err:
	final_lux_val = luxdata_int;
	APS_DBG("err als_value_lux = 0x%x\n", luxdata_int);
	return luxdata_int;
}



/*----------------------------------------------------------------------------*/
int ltr579_get_addr(struct alsps_hw *hw, struct ltr579_i2c_addr *addr)
{
	/***
	if(!hw || !addr)
	{
		return -EFAULT;
	}
	addr->write_addr= hw->i2c_addr[0];
	***/
	return 0;
}


/*-----------------------------------------------------------------------------*/
void ltr579_eint_func(void)
{
	APS_FUN();

	struct ltr579_priv *obj = ltr579_obj;
	if(!obj)
	{
		return;
	}
	schedule_work(&obj->eint_work);
	//schedule_delayed_work(&obj->eint_work);
}



/*----------------------------------------------------------------------------*/
/*for interrup work mode support -- by liaoxl.lenovo 12.08.2011*/
int ltr579_setup_eint(struct i2c_client *client)
{
	APS_FUN();
	char boot_mode;
	struct ltr579_priv *obj = (struct ltr579_priv *)i2c_get_clientdata(client);        

	ltr579_obj = obj;
	mt_set_gpio_dir(GPIO_ALS_EINT_PIN, GPIO_DIR_IN);
	mt_set_gpio_mode(GPIO_ALS_EINT_PIN, GPIO_ALS_EINT_PIN_M_EINT);
	mt_set_gpio_pull_enable(GPIO_ALS_EINT_PIN, /*TRUE*/FALSE);
	mt_set_gpio_pull_select(GPIO_ALS_EINT_PIN, GPIO_PULL_UP);

	mt_eint_set_hw_debounce(CUST_EINT_ALS_NUM, CUST_EINT_ALS_DEBOUNCE_CN);
	mt_eint_registration(CUST_EINT_ALS_NUM, CUST_EINT_ALS_TYPE, ltr579_eint_func, 0);

	boot_mode = get_boot_mode();
	if (boot_mode == FACTORY_BOOT) {
		mt_eint_mask(CUST_EINT_ALS_NUM);
	}else{
		mt_eint_unmask(CUST_EINT_ALS_NUM);  
	}
	
    return 0;
}


/*----------------------------------------------------------------------------*/
static void ltr579_power(struct alsps_hw *hw, unsigned int on) 
{
	static unsigned int power_on = 0;

	APS_LOG("power %s\n", on ? "on" : "off");

	if(hw->power_id != POWER_NONE_MACRO)
	{
		if(power_on == on)
		{
			APS_LOG("ignore power control: %d\n", on);
		}
		else if(on)
		{
			if(!hwPowerOn(hw->power_id, hw->power_vol, "ltr579")) 
			{
				APS_ERR("power on fails!!\n");
			}
		}
		else
		{
			if(!hwPowerDown(hw->power_id, "ltr579")) 
			{
				APS_ERR("power off fail!!\n");   
			}
		}
	}
	power_on = on;
}

/*----------------------------------------------------------------------------*/
/*for interrup work mode support -- by liaoxl.lenovo 12.08.2011*/
static int ltr579_check_and_clear_intr(struct i2c_client *client) 
{
//***
	APS_FUN();

	int res,intp,intl;
	u8 buffer[2];	
	u8 temp;
		//if (mt_get_gpio_in(GPIO_ALS_EINT_PIN) == 1) /*skip if no interrupt*/	
		//	  return 0;
	
		buffer[0] = LTR579_MAIN_STATUS;
		res = i2c_master_send(client, buffer, 0x1);
		if(res <= 0)
		{
			goto EXIT_ERR;
		}
		res = i2c_master_recv(client, buffer, 0x1);
		if(res <= 0)
		{
			goto EXIT_ERR;
		}
		temp = buffer[0];
		res = 1;
		intp = 0;
		intl = 0;
		if(0 != (buffer[0] & 0x02))
		{
			res = 0;
			intp = 1;
		}
		if(0 != (buffer[0] & 0x10))
		{
			res = 0;
			intl = 1;		
		}
	
		if(0 == res)
		{
			if((1 == intp) && (0 == intl))
			{
				buffer[1] = buffer[0] & 0xfD;
				
			}
			else if((0 == intp) && (1 == intl))
			{
				buffer[1] = buffer[0] & 0xEF;
			}
			else
			{
				buffer[1] = buffer[0] & 0xED;
			}
			buffer[0] = LTR579_MAIN_STATUS;
			res = i2c_master_send(client, buffer, 0x2);
			if(res <= 0)
			{
				goto EXIT_ERR;
			}
			else
			{
				res = 0;
			}
		}
	
		return res;
	
	EXIT_ERR:
		APS_ERR("ltr579_check_and_clear_intr fail\n");
		return 1;

}
/*----------------------------------------------------------------------------*/


static int ltr579_check_intr(struct i2c_client *client) 
{
	APS_FUN();

	int res,intp,intl;
	u8 buffer[2];

	//if (mt_get_gpio_in(GPIO_ALS_EINT_PIN) == 1) /*skip if no interrupt*/  
	//    return 0;

	buffer[0] = LTR579_MAIN_STATUS;
	res = i2c_master_send(client, buffer, 0x1);
	if(res <= 0)
	{
		goto EXIT_ERR;
	}
	res = i2c_master_recv(client, buffer, 0x1);
	if(res <= 0)
	{
		goto EXIT_ERR;
	}
	res = 1;
	intp = 0;
	intl = 0;
	if(0 != (buffer[0] & 0x02))
	{
		res = 0;
		intp = 1;
	}
	if(0 != (buffer[0] & 0x10))
	{
		res = 0;
		intl = 1;		
	}

	return res;

EXIT_ERR:
	APS_ERR("ltr579_check_intr fail\n");
	return 1;
}

static int ltr579_clear_intr(struct i2c_client *client) 
{
	int res;
	u8 buffer[2];

	APS_FUN();
	
	buffer[0] = LTR579_MAIN_STATUS;
	res = i2c_master_send(client, buffer, 0x1);
	if(res <= 0)
	{
		goto EXIT_ERR;
	}
	res = i2c_master_recv(client, buffer, 0x1);
	if(res <= 0)
	{
		goto EXIT_ERR;
	}
	APS_DBG("buffer[0] = %d \n",buffer[0]);

#if 0	
	buffer[1] = buffer[0] & 0x01;
	buffer[0] = LTR579_MAIN_STATUS;

	res = i2c_master_send(client, buffer, 0x2);
	if(res <= 0)
	{
		goto EXIT_ERR;
	}
	else
	{
		res = 0;
	}
#endif

	return res;

EXIT_ERR:
	APS_ERR("ltr579_check_and_clear_intr fail\n");
	return 1;
}




static int ltr579_devinit(void)
{
	int res;
	int init_als_gain;
	u8 databuf[2];	

	struct i2c_client *client = ltr579_obj->client;

	struct ltr579_priv *obj = ltr579_obj;   
	
	mdelay(PON_DELAY);

	// Enable PS at startup

	
	res = ltr579_ps_enable();
	if (res < 0)
		goto EXIT_ERR;


	// Enable ALS to Full Range at startup
	init_als_gain = ALS_RANGE_18;//ALS_RANGE_3;
	als_gainrange = init_als_gain;//Set global variable

	res = ltr579_als_enable(init_als_gain);
	if (res < 0)
		goto EXIT_ERR;

	if((res = ltr579_setup_eint(client))!=0)
	{
		APS_ERR("setup eint: %d\n", res);
		return res;
	}

	res = 0;

	EXIT_ERR:
	APS_ERR("init dev: %d\n", res);
	return res;

}
/*----------------------------------------------------------------------------*/


static int ltr579_get_als_value(struct ltr579_priv *obj, u16 als)
{
	int idx;
	int invalid = 0;
	APS_DBG("als  = %d\n",als); 
	for(idx = 0; idx < obj->als_level_num; idx++)
	{
		if(als < obj->hw->als_level[idx])
		{
			break;
		}
	}
	
	if(idx >= obj->als_level_num)
	{
		APS_ERR("exceed range\n"); 
		idx = obj->als_level_num - 1;
	}
	
	if(1 == atomic_read(&obj->als_deb_on))
	{
		unsigned long endt = atomic_read(&obj->als_deb_end);
		if(time_after(jiffies, endt))
		{
			atomic_set(&obj->als_deb_on, 0);
		}
		
		if(1 == atomic_read(&obj->als_deb_on))
		{
			invalid = 1;
		}
	}

APS_DBG("idx  = %d\n",idx); 

	if(!invalid)
	{
		APS_DBG("ALS: %05d => %05d\n", als, obj->hw->als_value[idx]);	
		return obj->hw->als_value[idx];
	}
	else
	{
		APS_ERR("ALS: %05d => %05d (-1)\n", als, obj->hw->als_value[idx]);    
		return -1;
	}
}
/*----------------------------------------------------------------------------*/

static int ltr579_get_ps_value()
{
	int ps_flag;
	int val;
	int error=-1;
	int buffer=0;

	buffer = ltr579_i2c_read_reg(LTR579_MAIN_STATUS);
	APS_DBG("Main status = %d\n", buffer);
	if (buffer < 0){
	    
	    APS_DBG("MAIN status read error\n");
		return error;
	}

	ps_flag = buffer & 0x04;
	ps_flag = ps_flag >>2;
	if(ps_flag==1) //Near
	{
	 	intr_flag_value =1;
		val=0;
	}
	else if(ps_flag ==0) //Far
	{
		intr_flag_value =0;
		val=5;
	}
    APS_DBG("ps_flag = %d, val = %d\n", ps_flag,val);
    
	return val;
}

/*----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------*/
/*for interrup work mode support */
static void ltr579_eint_work(struct work_struct *work)
{
	struct ltr579_priv *obj = (struct ltr579_priv *)container_of(work, struct ltr579_priv, eint_work);
	int err;
	hwm_sensor_data sensor_data;
//	u8 buffer[1];
//	u8 reg_value[1];
	u8 databuf[2];
	int res = 0;
	int ps_value;
	APS_FUN();

	if(1 == is_timer_active)
	{
		cancel_delayed_work_sync(&ps_detect_work);
		is_timer_active = 0;
	}

	err = ltr579_check_intr(obj->client);
	if(err < 0)
	{
		APS_ERR("ltr579_eint_work check intrs: %d\n", err);
	}
	else
	{
		//get ps flag
		ps_value=ltr579_get_ps_value();
		if(ps_value < 0)
    		{
    		err = -1;
    		return;
    		}
		//clear ps interrupt
		obj->ps = ltr579_ps_read();
    		if(obj->ps < 0)
    		{
    			err = -1;
    			return;
    		}
		printk("obj->ps = %d,ps_value = %d\n",obj->ps,ps_value);	
		ps_report_interrupt_data(ps_value);
	}

	
	//ltr579_clear_intr(obj->client);
    mt_eint_unmask(CUST_EINT_ALS_NUM);       
}



/****************************************************************************** 
 * Function Configuration
******************************************************************************/
static int ltr579_open(struct inode *inode, struct file *file)
{
	file->private_data = ltr579_i2c_client;

	if (!file->private_data)
	{
		APS_ERR("null pointer!!\n");
		return -EINVAL;
	}
	
	return nonseekable_open(inode, file);
}
/*----------------------------------------------------------------------------*/
static int ltr579_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}
/*----------------------------------------------------------------------------*/


static int ltr579_unlocked_ioctl(struct file *file, unsigned int cmd,
       unsigned long arg)       
{
	struct i2c_client *client = (struct i2c_client*)file->private_data;
	struct ltr579_priv *obj = i2c_get_clientdata(client);  
	int err = 0;
	void __user *ptr = (void __user*) arg;
	int dat;
	uint32_t enable;
    LTR579_THRE ltr579_threshold;
	APS_DBG("cmd= %d\n", cmd); 
	switch (cmd)
	{
		case ALSPS_SET_PS_MODE:
			if(copy_from_user(&enable, ptr, sizeof(enable)))
			{
				err = -EFAULT;
				goto err_out;
			}
			if(enable)
			{
			    err = ltr579_ps_enable();
				if(err < 0)
				{
					APS_ERR("enable ps fail: %d\n", err); 
					goto err_out;
				}
				set_bit(CMC_BIT_PS, &obj->enable);
			}
			else
			{
			    err = ltr579_ps_disable();
				if(err < 0)
				{
					APS_ERR("disable ps fail: %d\n", err); 
					goto err_out;
				}
				
				clear_bit(CMC_BIT_PS, &obj->enable);
			}
			break;

		case ALSPS_GET_PS_MODE:
			enable = test_bit(CMC_BIT_PS, &obj->enable) ? (1) : (0);
			if(copy_to_user(ptr, &enable, sizeof(enable)))
			{
				err = -EFAULT;
				goto err_out;
			}
			break;

		case ALSPS_GET_PS_DATA:
			APS_DBG("ALSPS_GET_PS_DATA\n"); 
		    obj->ps = ltr579_ps_read();
			if(obj->ps < 0)
			{
				goto err_out;
			}
			
			dat = ltr579_get_ps_value();
			if(copy_to_user(ptr, &dat, sizeof(dat)))
			{
				err = -EFAULT;
				goto err_out;
			}  
			break;

		case ALSPS_GET_PS_RAW_DATA:    
			obj->ps = ltr579_ps_read();
			if(obj->ps < 0)
			{
				goto err_out;
			}
			dat = obj->ps;
			if(copy_to_user(ptr, &dat, sizeof(dat)))
			{
				err = -EFAULT;
				goto err_out;
			}  
			break;

		case ALSPS_SET_ALS_MODE:
			if(copy_from_user(&enable, ptr, sizeof(enable)))
			{
				err = -EFAULT;
				goto err_out;
			}
			if(enable)
			{
			    err = ltr579_als_enable(als_gainrange);
				if(err < 0)
				{
					APS_ERR("enable als fail: %d\n", err); 
					goto err_out;
				}
				set_bit(CMC_BIT_ALS, &obj->enable);
			}
			else
			{
			    err = ltr579_als_disable();
				if(err < 0)
				{
					APS_ERR("disable als fail: %d\n", err); 
					goto err_out;
				}
				clear_bit(CMC_BIT_ALS, &obj->enable);
			}
			break;

		case ALSPS_GET_ALS_MODE:
			enable = test_bit(CMC_BIT_ALS, &obj->enable) ? (1) : (0);
			if(copy_to_user(ptr, &enable, sizeof(enable)))
			{
				err = -EFAULT;
				goto err_out;
			}
			break;

		case ALSPS_GET_ALS_DATA: 
		    obj->als = ltr579_als_read(als_gainrange);
			if(obj->als < 0)
			{
				goto err_out;
			}

			dat = ltr579_get_als_value(obj, obj->als);
			if(copy_to_user(ptr, &dat, sizeof(dat)))
			{
				err = -EFAULT;
				goto err_out;
			}              
			break;

		case ALSPS_GET_ALS_RAW_DATA:    
			obj->als = ltr579_als_read(als_gainrange);
			if(obj->als < 0)
			{
				goto err_out;
			}

			dat = (obj->als*als_test2real_rate1)/als_test2real_rate2;
			if(copy_to_user(ptr, &dat, sizeof(dat)))
			{
				err = -EFAULT;
				goto err_out;
			}              
			break;

        case ALSPS_IOCTL_SET_PS_CALI:
			if(copy_from_user(&ltr579_threshold, ptr, sizeof(ltr579_threshold)))
			{
				err = -EFAULT;
				goto err_out;
			}
	    APS_ERR("Alvin:ALSPS_IOCTL_SET_PS_CALI:result_als_cali_value  %d\n", ltr579_threshold.result_als_cali_value); 
	    APS_ERR("Alvin:ALSPS_IOCTL_SET_PS_CALI:result_ps_cali_value  %d\n", ltr579_threshold.result_ps_cali_value); 
            als_cali_multiply_num = ltr579_threshold.result_als_cali_value;
            break;
		default:
			APS_ERR("%s not supported = 0x%04x\n", __FUNCTION__, cmd);
			break;
	}

	err_out:
	return err;    
}

/*----------------------------------------------------------------------------*/
static struct file_operations ltr579_fops = {
	//.owner = THIS_MODULE,
	.open = ltr579_open,
	.release = ltr579_release,
	.unlocked_ioctl = ltr579_unlocked_ioctl,
};
/*----------------------------------------------------------------------------*/
static struct miscdevice ltr579_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "als_ps",
	.fops = &ltr579_fops,
};

static int ltr579_i2c_suspend(struct i2c_client *client, pm_message_t msg) 
{
	struct ltr579_priv *obj = i2c_get_clientdata(client);    
	int err;
	APS_FUN();    

	if(msg.event == PM_EVENT_SUSPEND)
	{   
		if(!obj)
		{
			APS_ERR("null pointer!!\n");
			return -EINVAL;
		}
		
		atomic_set(&obj->als_suspend, 1);
		err = ltr579_als_disable();
		if(err < 0)
		{
			APS_ERR("disable als: %d\n", err);
			return err;
		}

		atomic_set(&obj->ps_suspend, 1);
		err = ltr579_ps_disable();
		if(err < 0)
		{
			APS_ERR("disable ps:  %d\n", err);
			return err;
		}
		
		ltr579_power(obj->hw, 0);
	}
	return 0;
}
/*----------------------------------------------------------------------------*/
static int ltr579_i2c_resume(struct i2c_client *client)
{
	struct ltr579_priv *obj = i2c_get_clientdata(client);        
	int err;
	APS_FUN();

	if(!obj)
	{
		APS_ERR("null pointer!!\n");
		return -EINVAL;
	}

	ltr579_power(obj->hw, 1);
/*	err = ltr579_devinit();
	if(err < 0)
	{
		APS_ERR("initialize client fail!!\n");
		return err;        
	}*/
	atomic_set(&obj->als_suspend, 0);
	if(test_bit(CMC_BIT_ALS, &obj->enable))
	{
	    err = ltr579_als_enable(als_gainrange);
	    if (err < 0)
		{
			APS_ERR("enable als fail: %d\n", err);        
		}
	}
	atomic_set(&obj->ps_suspend, 0);
	if(test_bit(CMC_BIT_PS,  &obj->enable))
	{
		err = ltr579_ps_enable();
	    if (err < 0)
		{
			APS_ERR("enable ps fail: %d\n", err);                
		}
	}

	return 0;
}

static void ltr579_early_suspend(struct early_suspend *h) 
{   /*early_suspend is only applied for ALS*/
	struct ltr579_priv *obj = container_of(h, struct ltr579_priv, early_drv);   
	int err;
	APS_FUN();    
}

static void ltr579_late_resume(struct early_suspend *h)
{   /*early_suspend is only applied for ALS*/
	struct ltr579_priv *obj = container_of(h, struct ltr579_priv, early_drv);         
	int err;
	APS_FUN();
}



static int get_avg_lux(u16 lux)
{
	if(rct >= MAX_ELM)
		full=1;

	if(full){
		rct %= MAX_ELM;
		lux_sum -= record[rct];
	}
	lux_sum += lux;
	record[rct]=lux;
	rct++;

	return lux_sum / MAX_ELM;
}

/*----------------------------------------------------------------------------*/
static int ltr579_i2c_detect(struct i2c_client *client, int kind, struct i2c_board_info *info) 
{    
	strcpy(info->type, LTR579_DEV_NAME);
	return 0;
}

static void ps_detect_work_func(struct work_struct *work)
{
	int ps_raw_data = 0;
	int ps_val = 0;
	hwm_sensor_data sensor_data;
	int flag = intr_flag_value;

	ps_raw_data = ltr579_ps_read();
	if(ps_raw_data > prox_thresh_high)    //near
	{
		ps_val = 0;
		intr_flag_value = 1;
	}
	else                                  //far
	{
		ps_val = 5;
		intr_flag_value =0;
	}
	if(flag != intr_flag_value)
	{
		APS_DBG("force report.\n");
		APS_DBG("intr_flag_value=%d\n",intr_flag_value);
		ps_report_interrupt_data(ps_val);
	}
	schedule_delayed_work(&ps_detect_work,poll_delay);
}
static int ltr579_als_open_report_data(int open)
{
	//should queuq work to report event if  is_report_input_direct=true
	return 0;
}


// if use  this typ of enable , Gsensor only enabled but not report inputEvent to HAL

static int ltr579_als_enable_nodata(int en)
{
	int res = 0;

    APS_LOG("ltr579_obj als enable value = %d\n", en);

	if(!ltr579_obj)
	{
		APS_ERR("ltr579_obj is null!!\n");
		return -1;
	}

	if(en)
	{
	res=ltr579_als_enable(als_gainrange);
	if(res){
		APS_ERR("als_enable_nodata is failed!!\n");
		return -1;
		}
	}else{
		  res = ltr579_als_disable();
		if(res < 0)
		{
		APS_ERR("ltr579_als_enable_nodata disable als fail!\n"); 
		return -1;
		}
	}
	return 0;
}

static int ltr579_als_set_delay(u64 ns)
{
	//udelay(ns);
	return 0;
}

static int ltr579_als_get_data(int* value, int* status)
{
    struct ltr579_priv *obj = NULL;

	if(!ltr579_obj)
	{
		APS_ERR("cm3232_obj is null!!\n");
		return -1;
	}
	obj = ltr579_obj;
	
	*value = ltr579_als_read(als_gainrange);
	*status = SENSOR_STATUS_ACCURACY_MEDIUM;
	return 0;
}

static int ltr579_ps_open_report_data(int open)
{
	//should queuq work to report event if  is_report_input_direct=true
	return 0;
}


// if use  this typ of enable , Gsensor only enabled but not report inputEvent to HAL

static int ltr579_ps_enable_nodata(int en)
{
	int res = 0;

    	printk("ltr579_obj ps enable value = %d\n", en);

	if(!ltr579_obj)
	{
		APS_ERR("ltr579_obj is null!!\n");
		return -1;
	}
	if(en)
	{
		res=ltr579_ps_enable();
		if(res){
			APS_ERR("als_enable_nodata is failed!!\n");
			return -1;
		}
	}else{
		res = ltr579_ps_disable();
		if(res < 0)
		{
			APS_ERR("disable ps:  %d\n", res);
			return -1;
		}
	}
	return 0;
}

static int ltr579_ps_set_delay(u64 ns)
{
	//udelay(ns);
	return 0;
}

static int ltr579_ps_get_data(int* value, int* status)
{
    struct ltr579_priv *obj = NULL;
	int ps_raw_data = 0;
	if(!ltr579_obj)
	{
		APS_ERR("cm3232_obj is null!!\n");
		return -1;
	}
	obj = ltr579_obj;
	ps_raw_data = ltr579_ps_read();
	if(ps_raw_data > prox_thresh_high)    //near
	{
		*value = 0;
	}
	else                                  //far
	{
		*value = 5;
	}
	*status = SENSOR_STATUS_ACCURACY_MEDIUM;

	return 0;
}


/*----------------------------------------------------------------------------*/
static int ltr579_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct ltr579_priv *obj;
	struct hwmsen_object obj_ps, obj_als;
	struct als_control_path als_ctl={0};
	struct als_data_path als_data={0};
	struct ps_control_path ps_ctl={0};
	struct ps_data_path ps_data={0};
	int err = 0;

	if(!(obj = kzalloc(sizeof(*obj), GFP_KERNEL)))
	{
		err = -ENOMEM;
		goto exit;
	}
	memset(obj, 0, sizeof(*obj));
	ltr579_obj = obj;

	obj->hw = get_cust_alsps_hw();
	ltr579_get_addr(obj->hw, &obj->addr);

	INIT_WORK(&obj->eint_work, ltr579_eint_work);
	obj->client = client;
	i2c_set_clientdata(client, obj);	
	atomic_set(&obj->als_debounce, 300);
	atomic_set(&obj->als_deb_on, 0);
	atomic_set(&obj->als_deb_end, 0);
	atomic_set(&obj->ps_debounce, 300);
	atomic_set(&obj->ps_deb_on, 0);
	atomic_set(&obj->ps_deb_end, 0);
	atomic_set(&obj->ps_mask, 0);
	atomic_set(&obj->als_suspend, 0);
	atomic_set(&obj->ps_thd_val_high,  obj->hw->ps_threshold_high);
	atomic_set(&obj->ps_thd_val_low,  obj->hw->ps_threshold_low);
	//atomic_set(&obj->als_cmd_val, 0xDF);
	//atomic_set(&obj->ps_cmd_val,  0xC1);
	atomic_set(&obj->ps_thd_val,  obj->hw->ps_threshold);
	obj->enable = 0;
	obj->pending_intr = 0;
	obj->als_level_num = sizeof(obj->hw->als_level)/sizeof(obj->hw->als_level[0]);
	obj->als_value_num = sizeof(obj->hw->als_value)/sizeof(obj->hw->als_value[0]);   
	obj->als_modulus = (400*100)/(16*150);//(1/Gain)*(400/Tine), this value is fix after init ATIME and CONTROL register value
										//(400)/16*2.72 here is amplify *100
	BUG_ON(sizeof(obj->als_level) != sizeof(obj->hw->als_level));
	memcpy(obj->als_level, obj->hw->als_level, sizeof(obj->als_level));
	BUG_ON(sizeof(obj->als_value) != sizeof(obj->hw->als_value));
	memcpy(obj->als_value, obj->hw->als_value, sizeof(obj->als_value));
	atomic_set(&obj->i2c_retry, 3);
	set_bit(CMC_BIT_ALS, &obj->enable);
	set_bit(CMC_BIT_PS, &obj->enable);

	APS_LOG("ltr579_devinit() start...!\n");
	ltr579_i2c_client = client;


	//printk("@@@@@@ Part ID:%x\n",ltr579_i2c_read_reg(0x06));

	if(err = misc_register(&ltr579_device))
	{
		APS_ERR("ltr579_device register failed\n");
		goto exit_misc_device_register_failed;
	}

	
	/* Register sysfs attribute */
	if(err = ltr579_create_attr(&ltr579_init_info.platform_diver_addr->driver))
	{
		goto exit_create_attr_failed;
	}
	
	als_ctl.is_use_common_factory = false;
	ps_ctl.is_use_common_factory = false;
	als_ctl.open_report_data= ltr579_als_open_report_data;
	als_ctl.enable_nodata = ltr579_als_enable_nodata;
	als_ctl.set_delay  = ltr579_als_set_delay;
	als_ctl.is_report_input_direct = false;
	als_ctl.is_support_batch = false;
	err = als_register_control_path(&als_ctl);
	if(err)
	{
		goto exit_create_attr_failed;
	}else
		{
	}

	als_data.get_data = ltr579_als_get_data;
	als_data.vender_div = 100;
	err = als_register_data_path(&als_data);	
	if(err)
	{
		goto exit_create_attr_failed;
	}else
		{
	}

	ps_ctl.open_report_data = ltr579_ps_open_report_data;
	ps_ctl.enable_nodata = ltr579_ps_enable_nodata;
	ps_ctl.set_delay = ltr579_ps_set_delay;
	ps_ctl.is_report_input_direct = false;
	ps_ctl.is_support_batch = false;
	err = ps_register_control_path(&ps_ctl);	
	if(err)
	{
		APS_ERR("ps register control path fail = %d\n", err);
		goto exit_create_attr_failed;
	}

	ps_data.get_data = ltr579_ps_get_data;
	ps_data.vender_div = 100;
	err = ps_register_data_path(&ps_data);
	if(err)
	{
		APS_ERR("ps_register_data_path err = %d\n", err);
		goto exit_create_attr_failed;
	}

	if(err = ltr579_devinit())
	{
		goto exit_init_failed;
	}
	APS_LOG("ltr579_devinit() ...OK!\n");


#if defined(CONFIG_HAS_EARLYSUSPEND)
	obj->early_drv.level    = EARLY_SUSPEND_LEVEL_DISABLE_FB - 1,
	obj->early_drv.suspend  = ltr579_early_suspend,
	obj->early_drv.resume   = ltr579_late_resume,    
	register_early_suspend(&obj->early_drv);
#endif
	ltr579_init_flag = 0;
	APS_LOG("%s: OK\n", __func__);
	return 0;

	exit_create_attr_failed:
	misc_deregister(&ltr579_device);
	exit_misc_device_register_failed:
	exit_init_failed:
	//i2c_detach_client(client);
	exit_kfree:
	kfree(obj);
	exit:
	ltr579_i2c_client = NULL;
	ltr579_init_flag = -1;
//	MT6516_EINTIRQMask(CUST_EINT_ALS_NUM);  /*mask interrupt if fail*/
	APS_ERR("%s: err = %d\n", __func__, err);
	return err;
}


/*----------------------------------------------------------------------------*/

static int ltr579_i2c_remove(struct i2c_client *client)
{
	int err;	
	if(err = ltr579_delete_attr(&ltr579_i2c_driver.driver))
	{
		APS_ERR("ltr579_delete_attr fail: %d\n", err);
	} 

	if(err = misc_deregister(&ltr579_device))
	{
		APS_ERR("misc_deregister fail: %d\n", err);    
	}
	
	ltr579_i2c_client = NULL;
	i2c_unregister_device(client);
	kfree(i2c_get_clientdata(client));

	return 0;
}



static int  ltr579_local_init(void)
{
    struct alsps_hw *hw = get_cust_alsps_hw();

	ltr579_power(hw, 1);
	if(i2c_add_driver(&ltr579_i2c_driver))
	{
		APS_ERR("add driver error\n");
		return -1;
	}
	
	if(-1 == ltr579_init_flag)
	{
		i2c_del_driver(&ltr579_i2c_driver);
	   return -1;
	}
	
	return 0;
}

static int ltr579_remove(struct platform_device *pdev)
{
	struct alsps_hw *hw = get_cust_alsps_hw();
	APS_FUN();    
	ltr579_power(hw, 0);    
	i2c_del_driver(&ltr579_i2c_driver);
	return 0;
}
#if 0
/*----------------------------------------------------------------------------*/
static int ltr579_probe(struct platform_device *pdev) 
{
	struct alsps_hw *hw = get_cust_alsps_hw();

	ltr579_power(hw, 1);
	//ltr579_force[0] = hw->i2c_num;
	//ltr579_force[1] = hw->i2c_addr[0];
	//APS_DBG("I2C = %d, addr =0x%x\n",ltr579_force[0],ltr579_force[1]);
	if(i2c_add_driver(&ltr579_i2c_driver))
	{
		APS_ERR("add driver error\n");
		return -1;
	} 
	return 0;
}
/*----------------------------------------------------------------------------*/

static struct platform_driver ltr579_alsps_driver =
{
	.probe      = ltr579_probe,
	.remove     = ltr579_remove,    
	.driver     = 
	{
		.name = "als_ps",
        #ifdef CONFIG_OF
		.of_match_table = alsps_of_match,
		#endif
	}
};

#endif
/*----------------------------------------------------------------------------*/

#ifdef CONFIG_OF
static const struct of_device_id alsps_of_match[] = {
	{ .compatible = "mediatek,als_ps", },
	{},
};
#endif

/*----------------------------------------------------------------------------*/
static int __init ltr579_init(void)
{
       	struct alsps_hw *hw = get_cust_alsps_hw();
	i2c_register_board_info(hw->i2c_num, &i2c_ltr579, 1);
	APS_LOG("ltr579_init ~\n");
	alsps_driver_add(&ltr579_init_info);
	return 0;
}
/*----------------------------------------------------------------------------*/
static void __exit ltr579_exit(void)
{
	APS_FUN();
	//platform_driver_unregister(&ltr579_alsps_driver);
}
/*----------------------------------------------------------------------------*/
module_init(ltr579_init);
module_exit(ltr579_exit);
/*----------------------------------------------------------------------------*/
MODULE_AUTHOR("XX Xx");
MODULE_DESCRIPTION("LTR-579ALS Driver");
MODULE_LICENSE("GPL");
/* Driver version v1.0 */

