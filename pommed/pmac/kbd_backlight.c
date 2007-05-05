/*
 * pommed - Apple laptops hotkeys handler daemon
 *
 * $Id$
 *
 * Copyright (C) 2006-2007 Julien BLACHE <jb@jblache.org>
 * Copyright (C) 2006 Yves-Alexis Perez <corsac@corsac.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */ 


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>

#include <syslog.h>

#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>

#include <linux/adb.h>

#include <ofapi/of_api.h>

#include "../pommed.h"
#include "../conffile.h"
#include "../kbd_backlight.h"
#include "../ambient.h"
#include "../dbus.h"


#define SYSFS_I2C_BASE      "/sys/class/i2c-dev"
#define I2C_ADAPTER_NAME    "uni-n 0"

# define ADB_DEVICE          "/dev/adb"
# define ADB_BUFFER_SIZE     32


struct _lmu_info lmu_info;
struct _kbd_bck_info kbd_bck_info;


int
has_kbd_backlight(void)
{
  return ((mops->type == MACHINE_POWERBOOK_51)
	  || (mops->type == MACHINE_POWERBOOK_52)
	  || (mops->type == MACHINE_POWERBOOK_53)
	  || (mops->type == MACHINE_POWERBOOK_54)
	  || (mops->type == MACHINE_POWERBOOK_55)
	  || (mops->type == MACHINE_POWERBOOK_56)
	  || (mops->type == MACHINE_POWERBOOK_57)
	  || (mops->type == MACHINE_POWERBOOK_58)
	  || (mops->type == MACHINE_POWERBOOK_59));
}

static int
kbd_backlight_get(void)
{
  return kbd_bck_info.level;
}


static void
kbd_lmu_backlight_set(int val, int who)
{
  int curval;

  int i;
  float fadeval;
  float step;
  struct timespec fade_step;

  int fd;
  int ret;
  unsigned char buf[8];

  if (kbd_bck_info.inhibit ^ KBD_INHIBIT_CFG)
    return;

  if (lmu_info.lmuaddr == 0)
    return;

  curval = kbd_backlight_get();

  if (val == curval)
    return;

  if ((val < KBD_BACKLIGHT_OFF) || (val > KBD_BACKLIGHT_MAX))
    return;

  fd = open(lmu_info.i2cdev, O_RDWR);
  if (fd < 0)
    {
      logmsg(LOG_ERR, "Could not open %s: %s\n", lmu_info.i2cdev, strerror(errno));

      return;
    }

  ret = ioctl(fd, I2C_SLAVE, lmu_info.lmuaddr);
  if (ret < 0)
    {
      logmsg(LOG_ERR, "Could not ioctl the i2c bus: %s\n", strerror(errno));

      close(fd);
      return;
    }

  buf[0] = 0x01;   /* i2c register */

  if (who == KBD_AUTO)
    {
      fade_step.tv_sec = 0;
      fade_step.tv_nsec = (KBD_BACKLIGHT_FADE_LENGTH / KBD_BACKLIGHT_FADE_STEPS) * 1000000;

      fadeval = (float)curval;
      step = (float)(val - curval) / (float)KBD_BACKLIGHT_FADE_STEPS;

      for (i = 0; i < KBD_BACKLIGHT_FADE_STEPS; i++)
	{
	  fadeval += step;

	  /* See below for the format */
	  buf[1] = (unsigned char) fadeval >> 4;
	  buf[2] = (unsigned char) fadeval << 4;

	  if (write (fd, buf, 3) < 0)
	    {
	      logmsg(LOG_ERR, "Could not set LMU kbd brightness: %s\n", strerror(errno));

	      continue;
	    }

	  logdebug("KBD backlight value faded to %d\n", (int)fadeval);

	  nanosleep(&fade_step, NULL);
	}
    }
  
  /* The format appears to be: (taken from pbbuttonsd)
   *          byte 1   byte 2
   *         |<---->| |<---->|
   *         xxxx7654 3210xxxx
   *             |<----->|
   *                 ^-- brightness
   */
  
  buf[1] = (unsigned char) val >> 4;
  buf[2] = (unsigned char) val << 4;

  if (write (fd, buf, 3) < 0)
    logmsg(LOG_ERR, "Could not set LMU kbd brightness: %s\n", strerror(errno));

  close(fd);

  mbpdbus_send_kbd_backlight(val, kbd_bck_info.level, who);

  kbd_bck_info.level = val;
}

static void
kbd_pmu_backlight_set(int val, int who)
{
  int curval;

  int i;
  float fadeval;
  float step;
  struct timespec fade_step;

  int fd;
  int ret;
  unsigned char buf[ADB_BUFFER_SIZE];

  if (kbd_bck_info.inhibit ^ KBD_INHIBIT_CFG)
    return;

  curval = kbd_backlight_get();

  if (val == curval)
    return;

  if ((val < KBD_BACKLIGHT_OFF) || (val > KBD_BACKLIGHT_MAX))
    return;

  fd = open(ADB_DEVICE, O_RDWR);
  if (fd < 0)
    {
      logmsg(LOG_ERR, "Could not open %s: %s\n", ADB_DEVICE, strerror(errno));

      return;
    }

  if (who == KBD_AUTO)
    {
      fade_step.tv_sec = 0;
      fade_step.tv_nsec = (KBD_BACKLIGHT_FADE_LENGTH / KBD_BACKLIGHT_FADE_STEPS) * 1000000;

      fadeval = (float)curval;
      step = (float)(val - curval) / (float)KBD_BACKLIGHT_FADE_STEPS;

      for (i = 0; i < KBD_BACKLIGHT_FADE_STEPS; i++)
	{
	  fadeval += step;

	  buf[0] = PMU_PACKET;
	  buf[1] = 0x4f; /* PMU command */
	  buf[2] = 0;
	  buf[3] = 0;
	  buf[4] = (unsigned char)fadeval;

	  ret = write(fd, buf, 5);
	  if (ret != 5)
	    {
	      logmsg(LOG_ERR, "Could not set PMU kbd brightness: %s\n", strerror(errno));

	      continue;
	    }

	  ret = read(fd, buf, ADB_BUFFER_SIZE);
	  if (ret < 0)
	    {
	      logmsg(LOG_ERR, "Could not read PMU reply: %s\n", strerror(errno));

	      continue;
	    }

	  logdebug("KBD backlight value faded to %d\n", (int)fadeval);

	  nanosleep(&fade_step, NULL);
	}
    }
  
  buf[0] = PMU_PACKET;
  buf[1] = 0x4f; /* PMU command */
  buf[2] = 0;
  buf[3] = 0;
  buf[4] = val;

  ret = write(fd, buf, 5);
  if (ret != 5)
    {
      logmsg(LOG_ERR, "Could not set PMU kbd brightness: %s\n", strerror(errno));
    }
  else
    {
      ret = read(fd, buf, ADB_BUFFER_SIZE);
      if (ret < 0)
	logmsg(LOG_ERR, "Could not read PMU reply: %s\n", strerror(errno));
    }

  close(fd);

  mbpdbus_send_kbd_backlight(val, kbd_bck_info.level, who);

  kbd_bck_info.level = val;
}

static void
kbd_backlight_set(int val, int who)
{
  if ((mops->type == MACHINE_POWERBOOK_58)
      || (mops->type == MACHINE_POWERBOOK_59))
    {
      kbd_pmu_backlight_set(val, who);
    }
  else
    {
      kbd_lmu_backlight_set(val, who);
    }
}


void
kbd_backlight_step(int dir)
{
  int val;
  int newval;

  if (kbd_bck_info.inhibit ^ KBD_INHIBIT_CFG)
    return;

  val = kbd_backlight_get();

  if (val < 0)
    return;

  if (dir == STEP_UP)
    {
      newval = val + kbd_cfg.step;

      if (newval > KBD_BACKLIGHT_MAX)
	newval = KBD_BACKLIGHT_MAX;

      logdebug("KBD stepping +%d -> %d\n", kbd_cfg.step, newval);
    }
  else if (dir == STEP_DOWN)
    {
      newval = val - kbd_cfg.step;

      if (newval < KBD_BACKLIGHT_OFF)
	newval = KBD_BACKLIGHT_OFF;

      logdebug("KBD stepping -%d -> %d\n", kbd_cfg.step, newval);
    }
  else
    return;

  kbd_backlight_set(newval, KBD_USER);
}


/* Include automatic backlight routines */
#include "../kbd_auto.c"


static int 
kbd_probe_lmu(void);

void
kbd_backlight_init(void)
{
  int ret;

  if (kbd_cfg.auto_on)
    kbd_bck_info.inhibit = 0;
  else
    kbd_bck_info.inhibit = KBD_INHIBIT_CFG;

  kbd_bck_info.toggle_lvl = kbd_cfg.auto_lvl;

  kbd_bck_info.inhibit_lvl = 0;

  kbd_bck_info.auto_on = 0;

  if ((mops->type == MACHINE_POWERBOOK_58)
      || (mops->type == MACHINE_POWERBOOK_59))
    {
      /* Nothing to probe for the PMU05 machines */
      ret = 0;
    }
  else
    ret = kbd_probe_lmu();

  if ((!has_kbd_backlight()) || (ret < 0))
    {
      lmu_info.lmuaddr = 0;

      kbd_bck_info.r_sens = 0;
      kbd_bck_info.l_sens = 0;

      kbd_bck_info.level = 0;

      ambient_info.left = 0;
      ambient_info.right = 0;
      ambient_info.max = 0;

      return;
    }

  kbd_bck_info.level = kbd_backlight_get();

  if (kbd_bck_info.level < 0)
    kbd_bck_info.level = 0;

  kbd_bck_info.max = KBD_BACKLIGHT_MAX;

  ambient_init(&kbd_bck_info.r_sens, &kbd_bck_info.l_sens);
}


void
kbd_backlight_fix_config(void)
{
  if (kbd_cfg.auto_lvl > KBD_BACKLIGHT_MAX)
    kbd_cfg.auto_lvl = KBD_BACKLIGHT_MAX;

  if (kbd_cfg.step < 1)
    kbd_cfg.step = 1;

  if (kbd_cfg.step > (KBD_BACKLIGHT_MAX / 2))
    kbd_cfg.step = KBD_BACKLIGHT_MAX / 2;
}


static int
kbd_get_i2cdev(void)
{
  char buf[PATH_MAX];
  int i2c_bus;
  int ret;

  FILE *fp;

  /* All the 256 minors (major 89) are reserved for i2c adapters */
  for (i2c_bus = 0; i2c_bus < 256; i2c_bus++)
    {
      ret = snprintf(buf, PATH_MAX - 1, "%s/i2c-%d/name", SYSFS_I2C_BASE, i2c_bus);
      if ((ret < 0) || (ret >= (PATH_MAX - 1)))
	{
	  logmsg(LOG_WARNING, "Error: i2c device probe: device path too long");

	  i2c_bus = 256;
	  break;
	}

      fp = fopen(buf, "r");
      if ((fp == NULL) && (errno != ENOENT))
	{
	  logmsg(LOG_ERR, "Error: i2c device probe: cannot open %s: %s", buf, strerror(errno));
	  continue;
	}

      ret = fread(buf, 1, PATH_MAX - 1, fp);
      fclose(fp);

      if (ret < 1)
	continue;

      buf[ret - 1] = '\0';

      logdebug("Found i2c adapter [%s]\n", buf);

      if (ret < strlen(I2C_ADAPTER_NAME))
	continue;

      if (strncmp(buf, I2C_ADAPTER_NAME, strlen(I2C_ADAPTER_NAME)) == 0)
	{
	  logmsg(LOG_INFO, "Found %s i2c adapter at i2c-%d", I2C_ADAPTER_NAME, i2c_bus);
	  break;
	}
    }

  if (i2c_bus > 255)
    return -1;

  ret = snprintf(lmu_info.i2cdev, sizeof(lmu_info.i2cdev) - 1, "/dev/i2c-%d", i2c_bus);
  if ((ret < 0) || (ret >= (sizeof(lmu_info.i2cdev) - 1)))
    {
      logmsg(LOG_WARNING, "Error: i2c device path too long");

      return -1;
    }

  return 0;
}

int
kbd_get_lmuaddr(void)
{
  struct device_node *node;
  int plen;
  unsigned long *reg = NULL;

  of_init();

  node = of_find_node_by_type("lmu-controller", 0);
  if (node == NULL)
    {
      logmsg(LOG_ERR, "Error: no lmu-controller found in device-tree");

      return -1;
    }

  reg = of_find_property(node, "reg", &plen);
  lmu_info.lmuaddr = (unsigned int) (*reg >> 1);

  free(reg);
  of_free_node(node);

  logdebug("Found LMU controller at address 0x%x\n", lmu_info.lmuaddr);

  return 0;
}

static int
kbd_probe_lmu(void)
{
  int fd;
  int ret;
  char buffer[4];

  ret = kbd_get_lmuaddr();
  if (ret < 0)
    return -1;

  ret = kbd_get_i2cdev();
  if (ret < 0)
    return -1;

  fd = open(lmu_info.i2cdev, O_RDWR);
  if (fd < 0)
    {
      logmsg(LOG_WARNING, "Could not open device %s: %s\n", lmu_info.i2cdev, strerror(errno));

      return -1;
    }

  ret = ioctl(fd, I2C_SLAVE, lmu_info.lmuaddr);
  if (ret < 0)
    {
      logmsg(LOG_ERR, "ioctl failed on %s: %s\n", lmu_info.i2cdev, strerror(errno));

      close(fd);
      return -1;
    }

  ret = read(fd, buffer, 4);
  if (ret != 4)
    {
      logmsg(LOG_WARNING, "Probing failed on %s: %s\n", lmu_info.i2cdev, strerror(errno));

      close(fd);
      return -1;
    }
  close(fd);

  logdebug("Probing successful on %s\n", lmu_info.i2cdev);

  return 0;
}
