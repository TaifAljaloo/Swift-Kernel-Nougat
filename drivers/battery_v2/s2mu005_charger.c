/*
 * s2mu005_charger.c - S2MU005 Charger Driver
 *
 * Copyright (C) 2017 Samsung Electronics, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "include/charger/s2mu005_charger.h"
#include <linux/version.h>

#define ENABLE_MIVR 1

#define EN_OVP_IRQ 1
#define EN_IEOC_IRQ 1
#define EN_TOPOFF_IRQ 1
#define EN_RECHG_REQ_IRQ 0
#define EN_TR_IRQ 0
#define EN_MIVR_SW_REGULATION 0
#define EN_BST_IRQ 0
#define MINVAL(a, b) ((a <= b) ? a : b)

#define EOC_DEBOUNCE_CNT 2
#define HEALTH_DEBOUNCE_CNT 3
#define DEFAULT_CHARGING_CURRENT 500

#define EOC_SLEEP 200
#define EOC_TIMEOUT (EOC_SLEEP * 6)
#ifndef EN_TEST_READ
#define EN_TEST_READ 1
#endif

#define ENABLE 1
#define DISABLE 0

extern int factory_mode;

static enum power_supply_property sec_charger_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL,
	POWER_SUPPLY_PROP_CHARGING_ENABLED,
	POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION,
	POWER_SUPPLY_PROP_AUTHENTIC,
};

static enum power_supply_property s2mu005_otg_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

int otg_enable_flag;

static void s2mu005_set_fast_charging_current(struct i2c_client *i2c,
		int charging_current);
static int s2mu005_get_charging_health(struct s2mu005_charger_data *charger);

static void s2mu005_test_read(struct i2c_client *i2c)
{
	u8 data;
	char str[1016] = {0,};
	int i;

	for (i = 0x8; i <= 0x1A; i++) {
		s2mu005_read_reg(i2c, i, &data);

		sprintf(str+strlen(str), "0x%02x:0x%02x, ", i, data);
	}

	pr_info("[DEBUG]%s: %s\n", __func__, str);
}
static BLOCKING_NOTIFIER_HEAD(s2m_acok_notifier_list);

static int s2m_acok_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&s2m_acok_notifier_list, nb);
}

static int s2m_acok_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&s2m_acok_notifier_list, nb);
}

int s2m_acok_notify_call_chain(void)
{
	int ret = blocking_notifier_call_chain(&s2m_acok_notifier_list, 0, NULL);
	return notifier_to_errno(ret);
}
EXPORT_SYMBOL(s2m_acok_notify_call_chain);

static int s2m_acok_notifier_call(
				struct notifier_block *notifer,
				unsigned long event, void *v)
{
	struct power_supply *psy = get_power_supply_by_name("s2mu005-charger");
	struct s2mu005_charger_data *charger = power_supply_get_drvdata(psy);

	pr_info("s2m acok noti!!\n");
	/* Delay 100ms for debounce */
	queue_delayed_work(charger->charger_wqueue, &charger->charger_work, msecs_to_jiffies(100));
	return true;
}

struct notifier_block s2m_acok_notifier = {
	.notifier_call = s2m_acok_notifier_call,
};

bool s2mu005_charger_check_otg_mode(struct s2mu005_charger_data *charger)
{
	bool otg_status = false;
	u8 chg_ctrl0;

	s2mu005_read_reg(charger->client, S2MU005_CHG_CTRL0, &chg_ctrl0);
	chg_ctrl0 &= 0xF8;

	if (chg_ctrl0 & (1 << S2MU005_CHARGER_REG_MODE_OTG)) {
		otg_status = true;
	} else {
		otg_status = false;
	}
	pr_debug("[DEBUG]%s: OTG STATUS %d\n", __func__, otg_status);
	return otg_status;
}

static void s2mu005_charger_otg_control(struct s2mu005_charger_data *charger,
		bool enable)
{
	otg_enable_flag = enable;

	if (!enable) {
		/* set mode to Charger mode */
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL0,
			2 << REG_MODE_SHIFT, REG_MODE_MASK);

#ifdef CONFIG_SEC_FACTORY
		if(charger->dev_id >= 4) {
			/* set mode to Tx mode */
			s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL0,
				5 << REG_MODE_SHIFT, REG_MODE_MASK);

			mdelay(150);
			pr_info("%s: EVT4 OTG Control for factory mode\n", __func__);

			/* set mode to Charger mode */
			s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL0,
				2 << REG_MODE_SHIFT, REG_MODE_MASK);
		}
#endif

		/* mask VMID_INT */
		s2mu005_update_reg(charger->client, S2MU005_REG_SC_INT_MASK,
			1 << VMID_M_SHIFT, VMID_M_MASK);

		pr_info("%s : Turn off OTG\n",	__func__);
	} else {
		/* unmask VMID_INT */
		s2mu005_update_reg(charger->client, S2MU005_REG_SC_INT_MASK,
			0 << VMID_M_SHIFT, VMID_M_MASK);

#ifndef CONFIG_SEC_FACTORY
		/* EN_OTG OFF */
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL15,
			0 << T_EN_OTG_SHIFT, T_EN_OTG_MASK);
		/* OTG SCP disable */
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL4,
			1 << OTG_OCP_SW_ON_SHIFT, OTG_OCP_SW_ON_MASK);
#endif

		/* set mode to OTG */
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL0,
			4 << REG_MODE_SHIFT, REG_MODE_MASK);

#ifndef CONFIG_SEC_FACTORY
		msleep(5);

		/* EN_OTG ON */
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL15,
			1 << T_EN_OTG_SHIFT, T_EN_OTG_MASK);
		/* OTG SCP enable */
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL4,
			0 << OTG_OCP_SW_ON_SHIFT, OTG_OCP_SW_ON_MASK);
#endif

		/* set boost frequency to 1MHz */
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL11,
			2 << SET_OSC_BST_SHIFT, SET_OSC_BST_MASK);

		/* set OTG current limit to 1.5 A */
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL4,
			3 << SET_OTG_OCP_SHIFT, SET_OTG_OCP_MASK);

		/* VBUS switches are OFF when OTG over-current happen */
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL4,
			1 << OTG_OCP_SW_OFF_SHIFT, OTG_OCP_SW_OFF_MASK);

		/* set OTG voltage to 5.1 V */
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL5,
			0x16 << SET_VF_VMID_BST_SHIFT, SET_VF_VMID_BST_MASK);

		pr_info("%s : Turn on OTG\n",	__func__);
	}
#if EN_TEST_READ
	s2mu005_test_read(charger->client);
#endif
}

static void s2mu005_wdt_control(struct s2mu005_charger_data *charger,
		int onoff)
{
	u8 temp;

	if (onoff > 0) {
		s2mu005_read_reg(charger->client, 0x1A, &temp);
		temp &= ~0x3;
		temp |= 0x2;
		s2mu005_write_reg(charger->client, 0x1A, temp);
		pr_info("%s : Watchdog Timer Enabled,\n",	__func__);
	} else {
		s2mu005_read_reg(charger->client, 0x1A, &temp);
		temp &= ~0x3;
		temp |= 0x1;
		s2mu005_write_reg(charger->client, 0x1A, temp);
		pr_info("%s : Watchdog Timer Disabled,\n",	__func__);
	}
}

static void s2mu005_enable_charger_switch(struct s2mu005_charger_data *charger,
		int onoff)
{
	int buck_mode = 0;

	if (factory_mode) {
		pr_info("%s: Factory Mode Skip CHG_EN Control\n", __func__);
		return;
	}

	/* prevent vsys drop, set full current at QBAT */
	if (charger->dev_id <= 2) {
		s2mu005_set_fast_charging_current(charger->client, 1700);
		msleep(20);
	}

	if (charger->dev_id >= 4)
		buck_mode = 1;
	else
		buck_mode = 0;  // do not support buck only mode

	if (onoff > 0) {
		pr_info("[DEBUG]%s: turn on charger\n", __func__);
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL0,
			buck_mode << REG_MODE_SHIFT, REG_MODE_MASK);
		if(charger->dev_id <= 2) {
			msleep(50);
			s2mu005_update_reg(charger->client, 0x2A, 0 << 3, 0x08); // set async time 150msec
		}
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL0,
			2 << REG_MODE_SHIFT, REG_MODE_MASK);

		if(charger->dev_id <= 2) {
			msleep(150);
			s2mu005_update_reg(charger->client, 0x2A, 1 << 3, 0x08); // set async time 20msec recover
		}
		s2mu005_wdt_control(charger, 1); //watchdog timer enable
	} else {
		pr_info("[DEBUG] %s: turn off charger\n", __func__);
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL0,
			buck_mode << REG_MODE_SHIFT, REG_MODE_MASK);
		s2mu005_wdt_control(charger, 0); //watchdog timer disable
	}
}

static void s2mu005_set_buck(struct s2mu005_charger_data *charger, int enable)
{
	if (enable) {
		pr_info("[DEBUG]%s: set buck on\n", __func__);
		s2mu005_enable_charger_switch(charger, charger->is_charging);
	} else {
		pr_info("[DEBUG]%s: set buck off (charger off mode)\n", __func__);
		s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL0, 0 << REG_MODE_SHIFT, REG_MODE_MASK);
	}
}

static void s2mu005_set_regulation_voltage(struct s2mu005_charger_data *charger,
		int float_voltage)
{
	int data;

	if (factory_mode)
		return;

	pr_info("[DEBUG]%s: float_voltage %d \n", __func__, float_voltage);
	if (float_voltage <= 3900)
		data = 0;
	else if (float_voltage > 3900 && float_voltage <= 4400)
		data = (float_voltage - 3900) / 10;
	else
		data = 0x32;

	s2mu005_update_reg(charger->client,
		S2MU005_CHG_CTRL8, data << SET_VF_VBAT_SHIFT, SET_VF_VBAT_MASK);
}

static int s2mu005_get_regulation_voltage(struct s2mu005_charger_data *charger)
{
	u8 reg_data = 0;
	int float_voltage = 0;

	s2mu005_read_reg(charger->client, S2MU005_CHG_CTRL8, &reg_data);
	reg_data &= 0x7E;
	float_voltage = (reg_data >> SET_VF_VBAT_SHIFT) * 10 + 3900;
	pr_debug("%s: battery cv reg : 0x%x, float voltage val : %d\n",
		__func__, reg_data, float_voltage);

	return float_voltage;
}

static void s2mu005_set_input_current_limit(struct s2mu005_charger_data *charger,
		int charging_current)
{
	int data;

	if (factory_mode)
		return;

	pr_info("[DEBUG]%s: current  %d \n", __func__, charging_current);
	if (charging_current <= 100)
		data = 0;
	else if (charging_current >= 100 && charging_current <= 2600)
		data = (charging_current - 100) / 50;
	else
		data = 0x3F;

	s2mu005_update_reg(charger->client, S2MU005_CHG_CTRL2, data << INPUT_CURRENT_LIMIT_SHIFT,
			INPUT_CURRENT_LIMIT_MASK);
#if EN_TEST_READ
	s2mu005_test_read(charger->client);
#endif
}

static int s2mu005_get_input_current_limit(struct i2c_client *i2c)
{
	u8 data;
	int ret;

	ret = s2mu005_read_reg(i2c, S2MU005_CHG_CTRL2, &data);
	if (ret < 0)
		return ret;

	data = data & INPUT_CURRENT_LIMIT_MASK;

	if (data > 0x3F)
		data = 0x3F;
	return  data * 50 + 100;

}

static void s2mu005_set_fast_charging_current(struct i2c_client *i2c,
		int charging_current)
{
	int data;

	if (factory_mode)
		return;

	pr_info("[DEBUG]%s: current  %d \n", __func__, charging_current);
	if (charging_current <= 100)
		data = 0;
	else if (charging_current >= 100 && charging_current <= 2600)
		data = ((charging_current - 100) / 50) + 1;
	else
		data = 0x33;

	s2mu005_update_reg(i2c, S2MU005_CHG_CTRL7, data << FAST_CHARGING_CURRENT_SHIFT,
			FAST_CHARGING_CURRENT_MASK);

	/* work-around for unstable booting */
	if (data > 0x13) data = 0x13; /* 0x13 : 1A */
	s2mu005_update_reg(i2c, S2MU005_CHG_CTRL6, data << COOL_CHARGING_CURRENT_SHIFT,
	COOL_CHARGING_CURRENT_MASK); /* set cool charging current with max limit 1A */

#if EN_TEST_READ
	s2mu005_test_read(i2c);
#endif
}

static int s2mu005_get_fast_charging_current(struct i2c_client *i2c)
{
	u8 data;
	int ret;

	ret = s2mu005_read_reg(i2c, S2MU005_CHG_CTRL7, &data);
	if (ret < 0)
		return ret;

	data = data & FAST_CHARGING_CURRENT_MASK;

	if (data > 0x33)
		data = 0x33;
	return (data - 1 )* 50 + 100;
}

static int s2mu005_get_topoff_current(struct s2mu005_charger_data *charger)
{
	u8 data;
	int ret;

	ret = s2mu005_read_reg(charger->client, S2MU005_CHG_CTRL10, &data);
	if (ret < 0)
		return ret;

	data = data & FIRST_TOPOFF_CURRENT_MASK;

	if (data > 0x0F)
		data = 0x0F;
	return data * 25 + 100;
}

static void s2mu005_set_topoff_current(struct i2c_client *i2c,
		int eoc_1st_2nd, int current_limit)
{
	int data;

	pr_info("[DEBUG]%s: current  %d \n", __func__, current_limit);
	if (current_limit <= 100)
		data = 0;
	else if (current_limit > 100 && current_limit <= 475)
		data = (current_limit - 100) / 25;
	else
		data = 0x0F;

	switch(eoc_1st_2nd) {
	case 1:
		s2mu005_update_reg(i2c, S2MU005_CHG_CTRL10, data << FIRST_TOPOFF_CURRENT_SHIFT,
			FIRST_TOPOFF_CURRENT_MASK);
		break;
	case 2:
		s2mu005_update_reg(i2c, S2MU005_CHG_CTRL10, data << SECOND_TOPOFF_CURRENT_SHIFT,
			SECOND_TOPOFF_CURRENT_MASK);
		break;
	default:
		break;
	}
}

enum {
	S2MU005_MIVR_4200MV = 0,
	S2MU005_MIVR_4300MV,
	S2MU005_MIVR_4400MV,
	S2MU005_MIVR_4500MV,
	S2MU005_MIVR_4600MV,
	S2MU005_MIVR_4700MV,
	S2MU005_MIVR_4800MV,
	S2MU005_MIVR_4900MV,
};

#if ENABLE_MIVR
/* charger input regulation voltage setting */
static void s2mu005_set_mivr_level(struct s2mu005_charger_data *charger)
{
	int mivr = S2MU005_MIVR_4500MV;
	u8 temp = 0;

	s2mu005_read_reg(charger->client, 0x1A, &temp);
	temp |= 0x80;
	s2mu005_write_reg(charger->client, 0x1A, temp);

	s2mu005_update_reg(charger->client,
			S2MU005_CHG_CTRL1, mivr << SET_VIN_DROP_SHIFT, SET_VIN_DROP_MASK);
}
#endif /*ENABLE_MIVR*/

/* here is set init charger data */
#define S2MU003_MRSTB_CTRL 0X47
static bool s2mu005_chg_init(struct s2mu005_charger_data *charger)
{
	u8 temp;
	/* Read Charger IC Dev ID */
	s2mu005_read_reg(charger->client, S2MU005_REG_REV_ID, &temp);
	charger->dev_id = temp & 0x0F;

	dev_info(charger->dev, "%s : DEV ID : 0x%x\n", __func__,
			charger->dev_id);

/* s2mu005 : CHG 0xAF[7]=1 for SMPL issue, 0xAF[7]=0 for JIG case */
#if !defined(CONFIG_SEC_FACTORY)
	if (charger->dev_id == 3) {
		s2mu005_update_reg(charger->client, 0xAF, 1 << 7, 1 << 7);
		s2mu005_read_reg(charger->client, 0xAF, &temp);
		dev_info(charger->dev, "[DEBUG]%s : 0xAF(0x%x)\n", __func__, temp);
	}
#endif

	/* ready for self-discharge */
	s2mu005_update_reg(charger->client, S2MU005_REG_SELFDIS_CFG3,
			SELF_DISCHG_MODE_MASK, SELF_DISCHG_MODE_MASK);

#if !(ENABLE_MIVR)
	/* voltage regulatio disable does not exist mu005 */
#endif
	/* TOP-OFF debounce time set 256us */
	// only 003 ? need to check

	/* Disable (set 0min TOP OFF Timer) */
	// to be

	s2mu005_read_reg(charger->client, 0x7B, &temp);

	if(charger->dev_id <= 2) {
		s2mu005_update_reg(charger->client, 0x2A, 1 << 3, 0x08); // set async time 20msec recover
	}

	charger->fg_clock = temp;

	s2mu005_read_reg(charger->client, 0x20, &temp); //topoff timer 90min, watchdog timer 80sec
	temp &= ~0x3F;
	temp |= 0x35;
	s2mu005_write_reg(charger->client, 0x20, temp);

	s2mu005_read_reg(charger->client, S2MU005_CHG_CTRL0, &temp); //Always CHG_EN is ENABLED
	if( !(temp & 0x10) )
	{
		temp |= 0x10;
		s2mu005_write_reg(charger->client, S2MU005_CHG_CTRL0, temp);
		dev_info(charger->dev, "%s: CHG CTRL0 CHG EN : 0x%02x\n", __func__,temp);
	}

	/* float voltage */
	s2mu005_set_regulation_voltage(charger,
			charger->pdata->chg_float_voltage);

	dev_info(charger->dev, "%s: set float voltage : %d\n", __func__,charger->pdata->chg_float_voltage);

	s2mu005_read_reg(charger->client, 0x29, &temp); //Disable FC_CHG and PRE_CHG Timers
	temp &= 0x7F;
	s2mu005_write_reg(charger->client, 0x29, temp);

	return true;
}

static void s2mu005_charger_initialize(struct s2mu005_charger_data *charger)
{
	u8 temp = 0;

	s2mu005_read_reg(charger->client, 0x5A, &temp);
	temp |= 0x80;
	s2mu005_write_reg(charger->client, 0x5A, temp);

	if(charger->dev_id == 0) {
		s2mu005_write_reg(charger->client, 0x87, 0x00);
		s2mu005_write_reg(charger->client, 0x92, 0xE5);
		s2mu005_write_reg(charger->client, 0x97, 0x85);
		s2mu005_write_reg(charger->client, 0x9A, 0x67);
		s2mu005_write_reg(charger->client, 0x9C, 0xEA);
		s2mu005_write_reg(charger->client, 0x9E, 0x6E);
		s2mu005_write_reg(charger->client, 0xA1, 0x20);
		s2mu005_write_reg(charger->client, 0xA4, 0x0A);
		s2mu005_write_reg(charger->client, 0xA5, 0x45);

		s2mu005_read_reg(charger->client, 0x51, &temp);
		if(temp & 0x02) {
			s2mu005_read_reg(charger->client, 0x49, &temp);
			switch(temp & 0x1F) {
				case 0x18:
				case 0x19:
				case 0x1C:
				case 0x1D:
					break;
				default:
					s2mu005_read_reg(charger->client, 0x89, &temp);
					temp &= 0xFC;
					temp |= 0x01;
					s2mu005_write_reg(charger->client, 0x89, temp);
					break;
			}
		}
	}
	/* set fastest speed for QBAT switch */
	s2mu005_read_reg(charger->client, 0x87, &temp);
	temp &= ~0xF0;
	s2mu005_write_reg(charger->client, 0x87, temp);

	s2mu005_write_reg(charger->client, 0x27, 0x51);

	s2mu005_read_reg(charger->client, 0x20, &temp); //topoff timer 90min, watchdog timer 80sec
	temp &= ~0x3F;
	temp |= 0x35;
	s2mu005_write_reg(charger->client, 0x20, temp);

	s2mu005_write_reg(charger->client, 0x1A, 0x91);

	s2mu005_read_reg(charger->client, 0x13, &temp);
	temp &= ~0x60;
	s2mu005_write_reg(charger->client, 0x13, temp);

	s2mu005_read_reg(charger->client, 0xA8, &temp);
	temp &= 0x7F;
	temp |= 0x80;
	s2mu005_write_reg(charger->client, 0xA8, temp);

	s2mu005_write_reg(charger->client, 0x0F, 0x50);

	s2mu005_read_reg(charger->client, 0x89, &temp);
	temp &= ~0x80;
	s2mu005_write_reg(charger->client, 0x89, temp);

	s2mu005_read_reg(charger->client, 0xA5, &temp);
	temp &= ~0x04;
	s2mu005_write_reg(charger->client, 0xA5, temp);

	s2mu005_read_reg(charger->client, 0x20, &temp); //topoff timer 90min
	temp &= ~0x38;
	temp |= 0x30;
	s2mu005_write_reg(charger->client, 0x20, temp);

#if ENABLE_MIVR
	s2mu005_set_mivr_level(charger);
#endif /*DISABLE_MIVR*/
	/* float voltage */
	s2mu005_set_regulation_voltage(charger,
			charger->pdata->chg_float_voltage);
	/* topoff current */
	charger->topoff_current = 100;
	s2mu005_set_topoff_current(charger->client, 1, charger->topoff_current);
	if (charger->pdata->chg_eoc_dualpath) {
		s2mu005_set_topoff_current(charger->client, 2, charger->topoff_current);
	}

	dev_info(charger->dev, "%s: Re-initialize Charger completely\n", __func__);
}

static int s2mu005_get_charging_status(struct s2mu005_charger_data *charger)
{
	int status = POWER_SUPPLY_STATUS_UNKNOWN;
	int ret;
	u8 chg_sts;
	union power_supply_propval chg_mode;
	union power_supply_propval value;

	ret = s2mu005_read_reg(charger->client, S2MU005_CHG_STATUS0, &chg_sts);
	psy_do_property("battery", get, POWER_SUPPLY_PROP_CHARGE_NOW, chg_mode);
	psy_do_property("s2mu005-fuelgauge", get, POWER_SUPPLY_PROP_CURRENT_AVG, value);

	if (ret < 0)
		return status;

	switch (chg_sts & 0x0F) {
	case 0x00:	//charger is off
		status = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case 0x02:	//Pre-charge state
	case 0x03:	//Cool-charge state
	case 0x04:	//CC state
	case 0x05:	//CV state
		status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case 0x07:	//Top-off state
	case 0x06:	//Done Flag
	case 0x08:	//Done state
		dev_info(charger->dev, "%s: full check curr_avg(%d), topoff_curr(%d)\n",
			__func__, value.intval, charger->topoff_current);
		if (value.intval < charger->topoff_current)
			status = POWER_SUPPLY_STATUS_FULL;
		else
			status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case 0x0F:	//Input is invalid
		status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	default:
		break;
	}

#if EN_TEST_READ
	s2mu005_test_read(charger->client);
#endif
	return status;
}

static int s2mu005_get_charge_type(struct i2c_client *iic)
{
	int status = POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;
	u8 ret;

	s2mu005_read_reg(iic, S2MU005_CHG_STATUS0, &ret);
	if (ret < 0)
		dev_err(&iic->dev, "%s fail\n", __func__);

	switch (ret & CHG_OK_MASK ) {
	case CHG_OK_MASK:
		status = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	default:
		/* 005 does not need to do this */
		/* pre-charge mode */
		status = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	}

	return status;
}

static bool s2mu005_get_batt_present(struct i2c_client *iic)
{
	u8 ret;

	s2mu005_read_reg(iic, S2MU005_CHG_STATUS1, &ret);
	if (ret < 0)
		return false;

	return (ret & DET_BAT_STATUS_MASK) ? true : false;
}

static void s2mu005_wdt_clear(struct s2mu005_charger_data *charger)
{
	u8 status3;
	u8 ctrl13;

	s2mu005_read_reg(charger->client, S2MU005_CHG_STATUS3, &status3);

	s2mu005_read_reg(charger->client, S2MU005_CHG_CTRL13, &ctrl13);
	ctrl13 &= ~0x1;
	ctrl13 |= 0x1;
	s2mu005_write_reg(charger->client, S2MU005_CHG_CTRL13, ctrl13); /* wdt clear */

	status3 &= 0x0f;

	if (status3 == 0x05) {
		dev_info(&charger->client->dev,
			"%s: watchdog error status, enable charger\n", __func__);
		s2mu005_enable_charger_switch(charger, charger->is_charging);
	}
}

static int s2mu005_get_charging_health(struct s2mu005_charger_data *charger)
{
	u8 ret;
	int health = POWER_SUPPLY_HEALTH_GOOD;

	s2mu005_read_reg(charger->client, S2MU005_CHG_STATUS1, &ret);

	if (charger->is_charging) {
		s2mu005_wdt_clear(charger);
	}

	if (ret < 0) {
		pr_err("%s ERROR in reading health status\n", __func__);
		return health;
	}

	ret = (ret & 0x70) >> 4;
	switch (ret) {
	case 0x03:
	case 0x05:
		charger->ovp = false;
		charger->unhealth_cnt = 0;
		return health;
	default:
		break;
	}

	charger->unhealth_cnt++;
	if (charger->unhealth_cnt < HEALTH_DEBOUNCE_CNT) {
		return health;
	}

	/* 005 need to check ovp & health count */
	charger->unhealth_cnt = HEALTH_DEBOUNCE_CNT;
	if (charger->ovp) {
		health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		return health;
	}

	health = POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
	return health;
}

static int sec_chg_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct s2mu005_charger_data *charger = power_supply_get_drvdata(psy);
	int chg_curr, aicr;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = charger->charging_current ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = s2mu005_get_charging_status(charger);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = s2mu005_get_charging_health(charger);
#if EN_TEST_READ
		s2mu005_test_read(charger->client);
#endif
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = s2mu005_get_input_current_limit(charger->client);
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		if (charger->charging_current) {
			aicr = s2mu005_get_input_current_limit(charger->client);
			chg_curr = s2mu005_get_fast_charging_current(charger->client);
			val->intval = MINVAL(aicr, chg_curr);
		} else
			val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		val->intval = s2mu005_get_fast_charging_current(charger->client);
		break;
	case POWER_SUPPLY_PROP_CURRENT_FULL:
		val->intval = s2mu005_get_topoff_current(charger);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = s2mu005_get_charge_type(charger->client);
		break;
#if defined(CONFIG_BATTERY_SWELLING)
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = s2mu005_get_regulation_voltage(charger);
		break;
#endif
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = s2mu005_get_batt_present(charger->client);
		break;
	case POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL:
		val->intval = s2mu005_charger_check_otg_mode(charger);
		break;
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		val->intval = charger->is_charging;
		break;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int sec_chg_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct s2mu005_charger_data *charger = power_supply_get_drvdata(psy);
	enum power_supply_ext_property ext_psp = psp;
	int buck_state = ENABLE;
	union power_supply_propval value;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		charger->status = val->intval;
		break;
		/* val->intval : type */
	case POWER_SUPPLY_PROP_ONLINE:
		charger->cable_type = val->intval;
		charger->input_current = s2mu005_get_input_current_limit(charger->client);			
		pr_info("[DEBUG]%s:[BATT] cable_type(%d), input_current(%d)mA\n",
			__func__, charger->cable_type, charger->input_current);

		if (charger->cable_type != SEC_BATTERY_CABLE_OTG) {
			if (charger->cable_type == SEC_BATTERY_CABLE_NONE ||
					charger->cable_type == SEC_BATTERY_CABLE_UNKNOWN) {
				value.intval = 0;
			} else {
#if ENABLE_MIVR
				s2mu005_set_mivr_level(charger);
#endif 			/*DISABLE_MIVR*/
				value.intval = 1;
			}
			psy_do_property("s2mu005-fuelgauge", set, POWER_SUPPLY_PROP_ENERGY_AVG, value);
		}
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		{
			int input_current = val->intval;
			s2mu005_set_input_current_limit(charger, input_current);
			charger->input_current = val->intval;
		}
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		pr_info("[DEBUG] %s: is_charging %d\n", __func__, charger->is_charging);
		charger->charging_current = val->intval;
		/* set charging current */
		s2mu005_set_fast_charging_current(charger->client, charger->charging_current);
#if EN_TEST_READ
		s2mu005_test_read(charger->client);
#endif
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		break;
	case POWER_SUPPLY_PROP_CURRENT_FULL:
		charger->topoff_current = val->intval;
		if (charger->pdata->chg_eoc_dualpath) {
			s2mu005_set_topoff_current(charger->client, 1, val->intval);
			s2mu005_set_topoff_current(charger->client, 2, 100);
		}
		else
			s2mu005_set_topoff_current(charger->client, 1, val->intval);
		break;
#if defined(CONFIG_BATTERY_SWELLING)
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		pr_info("[DEBUG]%s: float voltage(%d)\n", __func__, val->intval);
		charger->pdata->chg_float_voltage = val->intval;
		s2mu005_set_regulation_voltage(charger,
				charger->pdata->chg_float_voltage);
		break;
#endif
	case POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL:
		s2mu005_charger_otg_control(charger, val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		charger->charge_mode = val->intval;
		psy_do_property("battery", get, POWER_SUPPLY_PROP_ONLINE, value);
		if (value.intval != SEC_BATTERY_CABLE_OTG) {
			pr_info("[DEBUG]%s: CHARGING_ENABLE(%d)\n", __func__, charger->charge_mode);
			switch (charger->charge_mode) {
			case SEC_BAT_CHG_MODE_BUCK_OFF:
				buck_state = DISABLE;
			case SEC_BAT_CHG_MODE_CHARGING_OFF:
				charger->is_charging = false;
				break;
			case SEC_BAT_CHG_MODE_CHARGING:
				charger->is_charging = true;
				break;
			}
			value.intval = charger->is_charging;
			psy_do_property("s2mu005-fuelgauge", set,
				POWER_SUPPLY_PROP_CHARGING_ENABLED, value);
			if (charger->dev_id >= 4) {
				if (buck_state) {
					s2mu005_enable_charger_switch(charger, charger->is_charging);
				} else {
					/* set buck off only if SEC_BAT_CHG_MODE_BUCK_OFF */
					s2mu005_set_buck(charger, buck_state);
				}
			} else {
				s2mu005_enable_charger_switch(charger, charger->is_charging);
			}
		} else {
			pr_info("[DEBUG]%s: SKIP CHARGING CONTROL while OTG(%d)\n",
				__func__, value.intval);
		}
		break;
	case POWER_SUPPLY_PROP_CHARGE_ENABLED:
		s2mu005_charger_initialize(charger);
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		{
#if !defined(CONFIG_SEC_FACTORY)
			u8 temp;
			if (charger->dev_id == 3) {
				if (val->intval) {
					s2mu005_update_reg(charger->client, 0xAF, 0 << 7, 1 << 7);
					s2mu005_read_reg(charger->client, 0xAF, &temp);
					pr_info("[DEBUG]%s: 0xAF(0x%x) (%d)\n", __func__, temp, val->intval);
				}
			}
#endif
			/* Switch-off charger if JIG is connected */
			if (val->intval && factory_mode) {
				pr_info("%s: JIG Connection status: %d \n", __func__, val->intval);
				s2mu005_enable_charger_switch(charger, false);
			}
		}
		break;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION:
		if (val->intval) {
			pr_info("%s: Relieve VBUS2BAT\n", __func__);
			s2mu005_write_reg(charger->client, 0x26, 0x5D);
		}
		break;
	case POWER_SUPPLY_PROP_AUTHENTIC:
		if (val->intval) {
			pr_info("%s: Bypass set\n", __func__);
			s2mu005_write_reg(charger->client, 0x2A, 0x10);
			s2mu005_write_reg(charger->client, 0x23, 0x15);
			s2mu005_write_reg(charger->client, 0x24, 0x44);
		}
		break;
	case POWER_SUPPLY_PROP_RESISTANCE:
		if(val->intval) {
			s2mu005_update_reg(charger->client, S2MU005_REG_SELFDIS_CFG2,
			FC_SELF_DISCHG_MASK, FC_SELF_DISCHG_MASK);
		} else {
			s2mu005_update_reg(charger->client, S2MU005_REG_SELFDIS_CFG2,
			0, FC_SELF_DISCHG_MASK);
		}
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		charger->fg_mode = val->intval;
		break;
	case POWER_SUPPLY_PROP_MAX ... POWER_SUPPLY_EXT_PROP_MAX:
		switch (ext_psp) {
		case POWER_SUPPLY_EXT_PROP_FUELGAUGE_RESET:
			s2mu005_write_reg(charger->client, 0x57, 0xC4);
			msleep(500);
			s2mu005_write_reg(charger->client, 0x57, 0x04);
			msleep(500);
			pr_info("%s: reset fuelgauge when surge occur! \n", __func__);
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int s2mu005_otg_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = otg_enable_flag;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int s2mu005_otg_set_property(struct power_supply *psy,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct s2mu005_charger_data *charger = power_supply_get_drvdata(psy);
	union power_supply_propval value;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		value.intval = val->intval;
		pr_info("%s: OTG %s\n", __func__, value.intval > 0 ? "on" : "off");
		psy_do_property(charger->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, value);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void s2mu005_det_bat_work(struct work_struct *work)
{
	struct s2mu005_charger_data *charger =
		container_of(work, struct s2mu005_charger_data, det_bat_work.work);
	u8 val;
	union power_supply_propval value;

	s2mu005_read_reg(charger->client, S2MU005_CHG_STATUS1, &val);
	if ((val & DET_BAT_STATUS_MASK) == 0)
	{
		//value.intval = 0;
		psy_do_property("s2mu005-fuelgauge", set, POWER_SUPPLY_PROP_CHARGE_EMPTY, value);
		s2mu005_enable_charger_switch(charger, 0);
		pr_info("charger-off if battery removed \n");
	}
}

/* s2mu005 interrupt service routine */
static irqreturn_t s2mu005_det_bat_isr(int irq, void *data)
{
	struct s2mu005_charger_data *charger = data;

	queue_delayed_work(charger->charger_wqueue, &charger->det_bat_work, 0);

	return IRQ_HANDLED;
}
#if 0
static irqreturn_t s2mu005_chg_isr(int irq, void *data)
{
	struct s2mu005_charger_data *charger = data;
	u8 val;

	s2mu005_read_reg(charger->client, S2MU005_CHG_STATUS0, &val);
	pr_info("[DEBUG] %s , %02x \n " , __func__, val);
	if ( val & (CHG_STATUS_DONE << CHG_STATUS_SHIFT) )
	{
		pr_info("add self chg done \n");
		/* add chg done code here */
	}
	return IRQ_HANDLED;
}
#endif
#if EN_OVP_IRQ
static void s2mu005_get_ovp_status(struct s2mu005_charger_data *charger)
{
	u8 val;
	union power_supply_propval value;

	s2mu005_read_reg(charger->client, S2MU005_CHG_STATUS1, &val);
	val = (val & VBUS_OVP_MASK) >> VBUS_OVP_SHIFT;
	if (val == 0x02) {
		charger->ovp = true;
		dev_info(charger->dev, "%s: OVP triggered, Vbus status: 0x%x\n", __func__, val);
		charger->unhealth_cnt = HEALTH_DEBOUNCE_CNT;
		value.intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		psy_do_property("battery", set,
			POWER_SUPPLY_PROP_HEALTH, value);
	} else if(val == 0x03 || val == 0x05) {
		if(charger->ovp) {
			dev_info(charger->dev, "%s: Recover from OVP, Vbus status 0x%x \n " , __func__, val);
			charger->unhealth_cnt = 0;
			charger->ovp = false;
			value.intval = POWER_SUPPLY_HEALTH_GOOD;
			psy_do_property("battery", set,
				POWER_SUPPLY_PROP_HEALTH, value);
		}
  	}
	charger->pending_chg_work = false;
}

static void s2mu005_ovp_work(struct work_struct *work)
{
	struct s2mu005_charger_data *charger =
		container_of(work, struct s2mu005_charger_data, charger_work.work);

	if (!charger->suspended) {
		s2mu005_get_ovp_status(charger);
	} else {
		charger->pending_chg_work = true;
	}
}
#endif

static int s2mu005_charger_parse_dt(struct device *dev,
		struct s2mu005_charger_platform_data *pdata)
{
	struct device_node *np = of_find_node_by_name(NULL, "s2mu005-charger");
	int ret;

	/* SC_CTRL11 , SET_OSC_BUCK , Buck switching frequency setting
		 * 0 : 500kHz
         * 1 : 750kHz
         * 2 : 1MHz
         * 3 : 2MHz
         */
/*	ret = of_property_read_u32(np,
		"battery,switching_frequency_mode", pdata->switching_frequency_mode);
	if (!ret)
		pdata->switching_frequency_mode = 1;
	pr_info("%s : switching_frequency_mode = %d\n", __func__,
			pdata->switching_frequency_mode);
*/
	/* SC_CTRL8 , SET_VF_VBAT , Battery regulation voltage setting */
	ret = of_property_read_u32(np, "battery,chg_float_voltage",
				&pdata->chg_float_voltage);

	np = of_find_node_by_name(NULL, "battery");
	if (!np) {
		pr_err("%s np NULL\n", __func__);
	} else {
		ret = of_property_read_string(np,
			"battery,charger_name", (char const **)&pdata->charger_name);

		ret = of_property_read_u32(np, "battery,full_check_type_2nd",
				&pdata->full_check_type_2nd);
		if (ret)
			pr_info("%s : Full check type 2nd is Empty\n", __func__);

		pdata->chg_eoc_dualpath = of_property_read_bool(np,
				"battery,chg_eoc_dualpath");

		pdata->always_enable = of_property_read_bool(np,
					"battery,always_enable");
	}

	dev_info(dev, "s2mu005 charger parse dt retval = %d\n", ret);
	return ret;
}

/* if need to set s2mu005 pdata */
static struct of_device_id s2mu005_charger_match_table[] = {
	{ .compatible = "samsung,s2mu005-charger",},
	{},
};

static const struct power_supply_desc s2mu005_charger_power_supply_desc = {
	.name = "s2mu005-charger",
	.type = POWER_SUPPLY_TYPE_UNKNOWN,
	.properties = sec_charger_props,
	.num_properties = ARRAY_SIZE(sec_charger_props),
	.get_property = sec_chg_get_property,
	.set_property = sec_chg_set_property,
};

static const struct power_supply_desc otg_power_supply_desc = {
	.name = "otg",
	.type = POWER_SUPPLY_TYPE_OTG,
	.properties = s2mu005_otg_props,
	.num_properties = ARRAY_SIZE(s2mu005_otg_props),
	.get_property = s2mu005_otg_get_property,
	.set_property = s2mu005_otg_set_property,
};

static int s2mu005_charger_probe(struct platform_device *pdev)
{
	struct s2mu005_dev *s2mu005 = dev_get_drvdata(pdev->dev.parent);
	struct s2mu005_platform_data *pdata = dev_get_platdata(s2mu005->dev);
	struct s2mu005_charger_data *charger;
	struct power_supply_config charger_cfg = {};
	int ret = 0;

	union power_supply_propval val;

	otg_enable_flag = 0;
	pr_info("%s:[BATT] S2MU005 Charger driver probe\n", __func__);
	charger = kzalloc(sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;

	mutex_init(&charger->io_lock);

	charger->dev = &pdev->dev;
	charger->client = s2mu005->i2c;

	charger->pdata = devm_kzalloc(&pdev->dev, sizeof(*(charger->pdata)),
			GFP_KERNEL);
	if (!charger->pdata) {
		dev_err(&pdev->dev, "Failed to allocate memory\n");
		ret = -ENOMEM;
		goto err_parse_dt_nomem;
	}
	ret = s2mu005_charger_parse_dt(&pdev->dev, charger->pdata);
	if (ret < 0)
		goto err_parse_dt;

	platform_set_drvdata(pdev, charger);

	if (charger->pdata->charger_name == NULL)
		charger->pdata->charger_name = "s2mu005-charger";

	s2mu005_chg_init(charger);

	charger_cfg.drv_data = charger;

	charger->psy_chg = power_supply_register(&pdev->dev, &s2mu005_charger_power_supply_desc, &charger_cfg);
	if (!charger->psy_chg) {
		dev_err(&pdev->dev, "%s: Failed to Register psy_chg\n", __func__);
		goto err_power_supply_register;
	}

	charger->psy_otg = power_supply_register(&pdev->dev, &otg_power_supply_desc, &charger_cfg);
	if (!charger->psy_otg) {
		dev_err(&pdev->dev, "%s: Failed to Register psy_otg\n", __func__);
		goto err_power_supply_register_otg;
	}

	charger->charger_wqueue = create_singlethread_workqueue("charger-wq");
	if (!charger->charger_wqueue) {
		dev_info(charger->dev, "%s: failed to create wq.\n", __func__);
		ret = -ESRCH;
		goto err_create_wq;
	}
	INIT_DELAYED_WORK(&charger->charger_work, s2mu005_ovp_work);
	INIT_DELAYED_WORK(&charger->det_bat_work, s2mu005_det_bat_work);

	/*
	 * irq request
	 * if you need to add irq , please refer below code.
	 */
	charger->irq_det_bat = pdata->irq_base + S2MU005_CHG_IRQ_DET_BAT;
	ret = request_threaded_irq(charger->irq_det_bat, NULL,
			s2mu005_det_bat_isr, 0 , "det-bat-in-irq", charger);
	if(ret < 0) {
		dev_err(s2mu005->dev, "%s: Fail to request det bat in IRQ: %d: %d\n",
					__func__, charger->irq_det_bat, ret);
		goto err_reg_irq;
	}
#if 0
	charger->irq_chg = pdata->irq_base + S2MU005_CHG_IRQ_CHG;
	ret = request_threaded_irq(charger->irq_chg, NULL,
			s2mu005_chg_isr, 0 , "chg-irq", charger);
	if(ret < 0) {
		dev_err(s2mu005->dev, "%s: Fail to request det bat in IRQ: %d: %d\n",
					__func__, charger->irq_chg, ret);
		goto err_reg_irq;
	}
#endif
	psy_do_property("s2mu005-fuelgauge", get, POWER_SUPPLY_PROP_SCOPE, val);
	charger->fg_mode = val.intval;

#if EN_TEST_READ
	s2mu005_test_read(charger->client);
#endif

	charger->suspended = false;
	charger->pending_chg_work = false;

	s2m_acok_register_notifier(&s2m_acok_notifier);

	pr_info("%s:[BATT] S2MU005 charger driver loaded OK\n", __func__);

	return 0;

err_reg_irq:
	destroy_workqueue(charger->charger_wqueue);
	power_supply_unregister(charger->psy_otg);
err_create_wq:
err_power_supply_register_otg:
	power_supply_unregister(charger->psy_chg);
err_power_supply_register:
err_parse_dt:
err_parse_dt_nomem:
	mutex_destroy(&charger->io_lock);
	kfree(charger);
	return ret;
}

static int s2mu005_charger_remove(struct platform_device *pdev)
{
	struct s2mu005_charger_data *charger =
		platform_get_drvdata(pdev);

	power_supply_unregister(charger->psy_chg);
	s2m_acok_unregister_notifier(&s2m_acok_notifier);
	mutex_destroy(&charger->io_lock);
	kfree(charger);
	return 0;
}

#if defined CONFIG_PM
static int s2mu005_charger_suspend(struct device *dev)
{
	struct s2mu005_charger_data *charger = dev_get_drvdata(dev);
	u8 data = 0;

	cancel_delayed_work_sync(&charger->charger_work);

	if (charger->dev_id < 2) {
		if (!charger->is_charging && !charger->fg_mode) {
			s2mu005_read_reg(charger->client, 0x72, &data);
			data |= 0x80;
			s2mu005_write_reg(charger->client, 0x72, data);

			data = charger->fg_clock + 64 > 0xFF ? 0xFF : charger->fg_clock + 64;
			s2mu005_write_reg(charger->client, 0x7B, data);
		}

		s2mu005_read_reg(charger->client, 0x7B, &data);
		pr_info("%s: 0x7B : 0x%x\n", __func__, data);
	}
	charger->suspended = true;
	return 0;
}

static int s2mu005_charger_resume(struct device *dev)
{
	struct s2mu005_charger_data *charger = dev_get_drvdata(dev);
	u8 data;

	if (charger->dev_id < 2) {
		if (!charger->is_charging && !charger->fg_mode) {
			s2mu005_read_reg(charger->client, 0x72, &data);
			data &= ~0x80;
			s2mu005_write_reg(charger->client, 0x72, data);

			s2mu005_write_reg(charger->client, 0x7B, charger->fg_clock);
		}
		s2mu005_read_reg(charger->client, 0x7B, &data);
		pr_info("%s: 0x7B : 0x%x\n", __func__, data);
	}

	if (charger->pending_chg_work) {
		s2mu005_get_ovp_status(charger);
	}

	charger->suspended = false;
	return 0;
}
#else
#define s2mu005_charger_suspend NULL
#define s2mu005_charger_resume NULL
#endif

static void s2mu005_charger_shutdown(struct platform_device *pdev)
{
	struct s2mu005_charger_data *charger = platform_get_drvdata(pdev);

	/*
	 * In case plug TA --> remove battery --> re-insert battery,
	 * we need to reset FG if SC_INT[0] = 1. However, it can make
	 * FG reset if plug TA --> power off --> LPM charging.
	 * To avoid the problem, when power-off sequence by power-key,
	 *    0x59[3]=0, 0x7C[0]=0 should be set in kernel.
	 *    0x59[3]=1, 0x7C[0]=1 should be set in bootloader.
	 */
	s2mu005_update_reg(charger->client, 0x59, 0, 0x01 << 3); /* manual reset disable */
	s2mu005_update_reg(charger->client, 0x7C, 0, 0x01 << 0); /* i2c port reset disable */

	pr_info("%s: S2MU005 Charger driver shutdown\n", __func__);

	if (!(charger->pdata->always_enable)) {
		pr_info("%s: turn on charger\n", __func__);
		s2mu005_enable_charger_switch(charger, true);
	}
}

static SIMPLE_DEV_PM_OPS(s2mu005_charger_pm_ops, s2mu005_charger_suspend,
		s2mu005_charger_resume);

static struct platform_driver s2mu005_charger_driver = {
	.driver         = {
		.name   = "s2mu005-charger",
		.owner  = THIS_MODULE,
		.of_match_table = s2mu005_charger_match_table,
		.pm     = &s2mu005_charger_pm_ops,
	},
	.probe          = s2mu005_charger_probe,
	.remove		= s2mu005_charger_remove,
	.shutdown	= s2mu005_charger_shutdown,
};

static int __init s2mu005_charger_init(void)
{
	int ret = 0;

	ret = platform_driver_register(&s2mu005_charger_driver);

	return ret;
}
module_init(s2mu005_charger_init);

static void __exit s2mu005_charger_exit(void)
{
	platform_driver_unregister(&s2mu005_charger_driver);
}
module_exit(s2mu005_charger_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Samsung Electronics");
MODULE_DESCRIPTION("Charger driver for S2MU005");
