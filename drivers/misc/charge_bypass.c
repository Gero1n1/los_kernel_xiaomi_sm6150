// SPDX-License-Identifier: GPL-2.0
/*
 * Charge bypass controller for Xiaomi SM7150 (sweet)
 *
 * Exposes:
 *   /sys/kernel/charge_bypass/enable   - 0 = normal charging, 1 = bypass
 *   /sys/kernel/charge_bypass/status   - human readable state
 *
 * Bypass is OFF by default. The driver only uses standard power_supply
 * properties already implemented by the bq2597x / ln8000 drivers in this
 * kernel; it never touches charger registers directly.
 *
 * NOTE: hardware bypass requires the charge-pump chip to report SC8551.
 * If the chip does not support it, only software "charging disable" is
 * applied (the phone then runs from the battery, not from the adapter).
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#define DRV_NAME "charge_bypass"

/* Both bq2597x and ln8000 drivers register a psy called
 * "bq2597x-standalone" in this kernel, so the second one gets a "-1"
 * suffix. Probe all known names instead of relying on ordering. */
static const char *const psy_names[] = {
	"bq2597x-standalone",
	"bq2597x-standalone-1",
	"bq2597x-master",
	"bq2597x-master-1",
	"bq2597x-slave",
	"bq2597x-slave-1",
	"ln8000_charger",
	"battery",
	NULL,
};

static struct kobject *bypass_kobj;
static DEFINE_MUTEX(bypass_lock);
static int bypass_enabled;
static int bypass_hw_ok;
static int bypass_sw_only;

static int psy_set(const char *name, enum power_supply_property psp, int val)
{
	struct power_supply *psy;
	union power_supply_propval pv = { .intval = val };
	int ret;

	psy = power_supply_get_by_name(name);
	if (!psy)
		return -ENODEV;

	ret = power_supply_set_property(psy, psp, &pv);
	power_supply_put(psy);
	return ret;
}

static void charge_bypass_apply(int enable)
{
	int i;

	mutex_lock(&bypass_lock);

	bypass_hw_ok = 0;
	bypass_sw_only = 0;

	for (i = 0; psy_names[i]; i++) {
		/* Enable/disable the SC8551 hardware bypass path if the
		 * charge-pump driver is present and supports the property. */
		if (!psy_set(psy_names[i], POWER_SUPPLY_PROP_TI_BYPASS_MODE_ENABLED,
			     enable)) {
			bypass_hw_ok = 1;
			psy_set(psy_names[i], POWER_SUPPLY_PROP_TI_CHARGE_MODE,
				enable ? 1 : 0); /* 1 = BYPASS, 0 = DIV2 */
		}
	}

	for (i = 0; psy_names[i]; i++) {
		/* Stop / resume battery charging on every charger that
		 * exposes this property (this is the safe software part). */
		if (!psy_set(psy_names[i], POWER_SUPPLY_PROP_CHARGING_ENABLED,
			     !enable))
			bypass_sw_only = 1;
	}

	bypass_enabled = enable;
	mutex_unlock(&bypass_lock);

	pr_info("%s: bypass %s (hw=%d sw=%d)\n", DRV_NAME,
		enable ? "enabled" : "disabled", bypass_hw_ok, bypass_sw_only);
}

static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", bypass_enabled);
}

static ssize_t enable_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	charge_bypass_apply(!!val);
	return count;
}

static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
		"bypass=%d hardware_bypass=%d charging_control=%d\n",
		bypass_enabled, bypass_hw_ok, bypass_sw_only);
}

static struct kobj_attribute enable_attr =
	__ATTR(enable, 0644, enable_show, enable_store);
static struct kobj_attribute status_attr =
	__ATTR(status, 0444, status_show, NULL);

static struct attribute *bypass_attrs[] = {
	&enable_attr.attr,
	&status_attr.attr,
	NULL,
};

static const struct attribute_group bypass_attr_group = {
	.attrs = bypass_attrs,
};

static int __init charge_bypass_init(void)
{
	int ret;

	bypass_kobj = kobject_create_and_add("charge_bypass", kernel_kobj);
	if (!bypass_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(bypass_kobj, &bypass_attr_group);
	if (ret) {
		kobject_put(bypass_kobj);
		return ret;
	}

	pr_info("%s: initialized (sysfs: /sys/kernel/charge_bypass)\n", DRV_NAME);
	return 0;
}

static void __exit charge_bypass_exit(void)
{
	sysfs_remove_group(bypass_kobj, &bypass_attr_group);
	kobject_put(bypass_kobj);
}

module_init(charge_bypass_init);
module_exit(charge_bypass_exit);

MODULE_AUTHOR("Gero1n1");
MODULE_DESCRIPTION("Charge bypass control for Xiaomi SM7150 (sweet)");
MODULE_LICENSE("GPL v2");
