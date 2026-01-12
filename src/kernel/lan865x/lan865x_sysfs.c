// SPDX-License-Identifier: GPL-2.0+
/*
 * LAN865x sysfs interface implementation
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/slab.h>

#include "lan865x_sysfs.h"

#define LAN865X_CLASS_NAME "lan865x"

static struct class *lan865x_class;
static dev_t lan865x_devt;
static int lan865x_minor = 0;

/* sysfs show function for node_id */
static ssize_t node_id_show(struct device *dev, struct device_attribute *attr,
                            char *buf)
{
    struct lan865x_priv *priv = dev_get_drvdata(dev);
    int node_id;

    if (!priv) {
        return -ENODEV;
    }

    node_id = lan865x_get_node_id(priv);
    if (node_id < 0) {
        return node_id;
    }

    return sprintf(buf, "%d\n", node_id);
}

/* Define device attribute for node_id (Read Only) */
static DEVICE_ATTR_RO(node_id);

/* Define attribute group */
static struct attribute *lan865x_attrs[] = {
    &dev_attr_node_id.attr,

    /* Reserved for future attributes */
    NULL,
};

static const struct attribute_group lan865x_attr_group = {
    .attrs = lan865x_attrs,
};

/* Initialize sysfs class */
int lan865x_sysfs_init(void)
{
    int ret;

    /* Allocate character device region */
    ret = alloc_chrdev_region(&lan865x_devt, 0, 1, LAN865X_CLASS_NAME);
    if (ret < 0) {
        pr_err("lan865x: failed to allocate chrdev region\n");
        return ret;
    }

    lan865x_class = class_create(LAN865X_CLASS_NAME);
    if (IS_ERR(lan865x_class)) {
        pr_err("lan865x: failed to create class\n");
        unregister_chrdev_region(lan865x_devt, 1);
        return PTR_ERR(lan865x_class);
    }

    return 0;
}

/* Cleanup sysfs class */
void lan865x_sysfs_exit(void)
{
    if (lan865x_class) {
        class_destroy(lan865x_class);
        lan865x_class = NULL;
    }

    unregister_chrdev_region(lan865x_devt, 1);
}

/* Create sysfs device for a lan865x instance */
int lan865x_sysfs_create_device(struct lan865x_priv *priv)
{
    struct device *dev;
    dev_t devt;
    char name[16];
    int ret;

    if (!lan865x_class) {
        pr_err("lan865x: class not initialized\n");
        return -ENODEV;
    }

    /* Allocate a minor number for this device */
    devt = MKDEV(MAJOR(lan865x_devt), lan865x_minor++);

    /* Create unique device name */
    snprintf(name, sizeof(name), "lan865x%d", MINOR(devt));

    /* Create device in the class */
    dev = device_create(lan865x_class, NULL, devt, priv, name);
    if (IS_ERR(dev)) {
        pr_err("lan865x: failed to create device\n");
        lan865x_minor--;
        return PTR_ERR(dev);
    }

    /* Create attribute group */
    ret = sysfs_create_group(&dev->kobj, &lan865x_attr_group);
    if (ret) {
        pr_err("lan865x: failed to create attribute group\n");
        device_destroy(lan865x_class, devt);
        lan865x_minor--;
        return ret;
    }

    return 0;
}

/* Remove sysfs device for a lan865x instance */
void lan865x_sysfs_remove_device(struct lan865x_priv *priv)
{
    struct device *dev;
    int minor;

    if (!lan865x_class) {
        return;
    }

    /* Find the device associated with this priv */
    /* Iterate through possible minor numbers */
    for (minor = 0; minor < lan865x_minor; minor++) {
        dev_t devt = MKDEV(MAJOR(lan865x_devt), minor);
        dev = class_find_device_by_devt(lan865x_class, devt);
        if (dev) {
            if (dev_get_drvdata(dev) == priv) {
                sysfs_remove_group(&dev->kobj, &lan865x_attr_group);
                device_destroy(lan865x_class, devt);
                put_device(dev);
                return;
            }
            put_device(dev);
        }
    }
}
