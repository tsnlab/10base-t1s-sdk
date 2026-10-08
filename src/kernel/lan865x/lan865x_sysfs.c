// SPDX-License-Identifier: GPL-3.0+
/*
 * LAN865x sysfs interface implementation
 *
 * Copyright (c) 2026 TSN Lab
 * Original Author: Jihoon Park <pakji@tsnlab.com>
 */

#include "lan865x_sysfs.h"
#include "lan865x_arch.h"

#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#define LAN865X_CLASS_NAME "lan865x"
#define LAN865X_MAX_DEVS 4

static struct class* lan865x_class;
static dev_t lan865x_devt;
static int lan865x_minor = 0;

/* The register node gives raw read/write access to every MAC-PHY register
 * (including clear-on-read statistics and reset control): without a udev
 * rule it is root only.  Access is decided by the node mode alone, so a
 * udev rule (the lab RPi4 has KERNEL=="lan865x[0-9]*", GROUP="tsnlab",
 * MODE="0660") can grant it to a group.
 */
static char* lan865x_devnode(const struct device* dev, umode_t* mode) {
    if (mode) {
        *mode = 0600;
    }
    return NULL;
}

/* sysfs show function for node_id */
static ssize_t node_id_show(struct device* dev, struct device_attribute* attr, char* buf) {
    struct lan865x_priv* priv = dev_get_drvdata(dev);
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

/* Side-effect free register snapshot (lan865x_health.c), world readable */
static ssize_t regdump_show(struct device* dev, struct device_attribute* attr, char* buf) {
    struct lan865x_priv* priv = dev_get_drvdata(dev);

    if (!priv) {
        return -ENODEV;
    }
    return lan865x_regdump(priv, buf);
}
static DEVICE_ATTR_RO(regdump);

/* Define attribute group */
static struct attribute* lan865x_attrs[] = {
    &dev_attr_node_id.attr,
    &dev_attr_regdump.attr,

    /* Reserved for future attributes */
    NULL,
};

static const struct attribute_group lan865x_attr_group = {
    .attrs = lan865x_attrs,
};

/* Initialize sysfs class */
int lan865x_sysfs_init(void) {
    int ret;

    /* Allocate character device region */
    ret = alloc_chrdev_region(&lan865x_devt, 0, LAN865X_MAX_DEVS, LAN865X_CLASS_NAME);
    if (ret < 0) {
        pr_err("lan865x: failed to allocate chrdev region\n");
        return ret;
    }

    lan865x_class = class_create(LAN865X_CLASS_NAME);
    if (IS_ERR(lan865x_class)) {
        pr_err("lan865x: failed to create class\n");
        unregister_chrdev_region(lan865x_devt, LAN865X_MAX_DEVS);
        return PTR_ERR(lan865x_class);
    }
    lan865x_class->devnode = lan865x_devnode;

    return 0;
}

/* Cleanup sysfs class */
void lan865x_sysfs_exit(void) {
    if (lan865x_class) {
        class_destroy(lan865x_class);
        lan865x_class = NULL;
    }

    unregister_chrdev_region(lan865x_devt, LAN865X_MAX_DEVS);
}

/* Create sysfs device (and the /dev/lan865xN register node) for a lan865x
 * instance.
 *
 * Before this change only device_create() was called: the class device and
 * the /dev node existed, but no cdev was bound to the dev_t, so every open()
 * failed with ENXIO and LAN865X_READ_REG/WRITE_REG could never be reached.
 * The cdev is added before device_create() so that the node is usable as soon
 * as udev/devtmpfs creates it.
 */
int lan865x_sysfs_create_device(struct lan865x_priv* priv, const struct file_operations* fops) {
    struct device* dev;
    dev_t devt;
    char name[16];
    int ret;

    if (!lan865x_class) {
        pr_err("lan865x: class not initialized\n");
        return -ENODEV;
    }

    if (lan865x_minor >= LAN865X_MAX_DEVS) {
        pr_err("lan865x: too many devices\n");
        return -ENOSPC;
    }

    /* Allocate a minor number for this device */
    devt = MKDEV(MAJOR(lan865x_devt), lan865x_minor++);
    priv->devt = devt;

    cdev_init(&priv->cdev, fops);
    priv->cdev.owner = THIS_MODULE;
    ret = cdev_add(&priv->cdev, devt, 1);
    if (ret) {
        pr_err("lan865x: failed to add cdev: %d\n", ret);
        lan865x_minor--;
        return ret;
    }
    priv->cdev_added = true;

    /* Create unique device name */
    snprintf(name, sizeof(name), "lan865x%d", MINOR(devt));

    /* Create device in the class */
    dev = device_create(lan865x_class, NULL, devt, priv, name);
    if (IS_ERR(dev)) {
        pr_err("lan865x: failed to create device\n");
        cdev_del(&priv->cdev);
        priv->cdev_added = false;
        lan865x_minor--;
        return PTR_ERR(dev);
    }

    /* Create attribute group */
    ret = sysfs_create_group(&dev->kobj, &lan865x_attr_group);
    if (ret) {
        pr_err("lan865x: failed to create attribute group\n");
        device_destroy(lan865x_class, devt);
        cdev_del(&priv->cdev);
        priv->cdev_added = false;
        lan865x_minor--;
        return ret;
    }

    return 0;
}

/* Remove sysfs device for a lan865x instance */
void lan865x_sysfs_remove_device(struct lan865x_priv* priv) {
    struct device* dev;
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
                if (priv->cdev_added) {
                    cdev_del(&priv->cdev);
                    priv->cdev_added = false;
                }
                return;
            }
            put_device(dev);
        }
    }
}
