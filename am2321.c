/*!
 *
 * Receive and print the value of temperature and humidity from AM2321 sold by Akizuki-denshi.
 *
 * @file am2321.c
 *
 * @date 2014/08/22
 * @author Kodai Tooi
 * @version 1.0
 */
#include "lib/i2c-ctl.h"
#include <unistd.h>

#if MODULE
  #include <linux/kernel.h>
  #include <linux/module.h>
  #include <linux/errno.h>
  #include <linux/fs.h>
#else
  #include <string.h>
  #include <stdio.h>
  #include <stdlib.h>
  //ユーザランドでも動くようにするための、関数・定数の再定義
  #define printk(...) fprintf(stderr, __VA_ARGS__)
  #define KERN_INFO ""
  #define KERN_NOTICE ""
  #define KERN_WARNING ""
  #define KERN_ERR ""
#endif

#define I2C_DEV "/dev/i2c-%d"

#define AM2321_ID 0x5c
#define AM2321_DEV_NAME "am2321"
#define I2C_SLAVE_MAX_RETRY 5
#define AM2321_WAIT_WAKEUP 800      // 800 to 3000
#define AM2321_WAIT_WRITEMODE 1500  // up to 1500
#define AM2321_WAIT_READMODE 30     // up to 30
#define AM2321_WAIT_REFRESH 2000000 // up to 2000000(= 2sec)

struct am2321 {

  char register_data[8];
  // register_data の取得時刻とか。
};

#if MODULE
static struct am2321 *cur_data;
#endif


/*!
 * @brief Set to write mode to AM2321.
 *
 * @param[in] fd File descriptor for AM2321.
 *
 * @return Successed : 0, Failed : -1
 */
int write_mode_am2321(I2CSlave *i2c_slave) {

  // write(fd, NULL, 0) を実行する。
  // この処理は、 AM2321 に 0xb8 を送信する。
  if (write_i2c_slave(i2c_slave, NULL, 0) == -1) {
    printk(KERN_ERR "am2321 : Failed set to write mode for am2321.\n");
    return -1;
  }
  return 0;
}

/*!
 * @brief Return AM2321 from suspend mode.
 *
 * @param[in] fd File descriptor for AM2321.
 *
 * @return Successed : 0, Failed : -1
 */
int wakeup_am2321(I2CSlave *i2c_slave) {

  if (write_mode_am2321(i2c_slave) == -1) {
    printk(KERN_ERR "am2321 : Failed wakeup am2321.\n");
    return -1;
  }
  usleep(AM2321_WAIT_WAKEUP);
  return 0;
}

/*!
 * @brief Calculate the value of temperature and humidity.
 *
 * @param[in] high High-order bit received from AM2321.
 * @param[in] low  Low-order bit received from AM2321.
 *
 * @return The value of calculated.
 */
double calc_data(char high, char low) {

  return ((high << 8) + low) / 10.0;
}

/*!
 * @brief Calculate the value of humidity.
 *
 * @param[in] am2321_data The data of received from AM2321.
 *
 * @return The value of calculated humidity.
 */
double calc_hum(struct am2321 *am2321_data) {

  return calc_data(am2321_data->register_data[2], am2321_data->register_data[3]);
}

/*!
 * @brief Calculate the value of temperature.
 *
 * @param[in] am2321_data The data of received from AM2321.
 *
 * @return The value of calculated temperature.
 */
double calc_temp(struct am2321 *am2321_data) {

  return calc_data(am2321_data->register_data[4], am2321_data->register_data[5]);
}

/*!
 * @brief Calcute the value of discomfort index from AM2321.
 * See : http://ja.wikipedia.org/wiki/%E4%B8%8D%E5%BF%AB%E6%8C%87%E6%95%B0
 *
 * @param[in] am2321_data The data of received from AM2321.
 *
 * @return The value of calculated discomfort index.
 */
double calc_discomfort(struct am2321 *am2321_data) {

  double hum, temp;
  hum = calc_hum(am2321_data);
  temp = calc_temp(am2321_data);

  return 0.81 * temp + 0.01 * hum * (0.99 * temp - 14.3) + 46.3;
}

/*!
 * @brief Check the error from received data from AM2321.
 *
 * @param[in] am2321_data The data of received from AM2321.
 *
 * @return The data is not error : 0, The data is error : -1
 */
int check_err(struct am2321 *am2321_data) {

  if (am2321_data->register_data[2] >= 0x80) {
    printk(KERN_ERR "am2321 : Received error code : 0x%0x\n", am2321_data->register_data[2]);
    return -1;
  } else {
    return 0;
  }
}

/*!
 * @brief Calculate and check the CRC from received data from AM2321.
 *
 * @param[in] am2321_data The data of received from AM2321.
 *
 * @return CRC check is OK : 0, CRC check is NG : -1
 */
int check_crc(struct am2321 *am2321_data) {

  int rcv_crcsum = (am2321_data->register_data[7] << 8) + am2321_data->register_data[6];
  int clc_crcsum = 0xffff;
  int i, j, ret;

  for (i = 0; i < 6; i++) {
    clc_crcsum ^= (unsigned short)am2321_data->register_data[i];
    for (j = 0; j < 8; j++) {
      if (clc_crcsum & 1) {
        clc_crcsum = clc_crcsum >> 1;
        clc_crcsum ^= 0xa001;
      } else {
        clc_crcsum = clc_crcsum >> 1;
      }
    }
  }

  if (rcv_crcsum == clc_crcsum) {
    ret = 0;
  } else {
    printk(KERN_NOTICE "am2321 : Failed CRC check sum. Receive CRC : 0x%0x, Calc CRC : 0x%0x\n", rcv_crcsum, clc_crcsum);
    ret = -1;
  }

  return ret;
}

/*!
 * @brief Measure the value of temperature, humidity and discomfort index from AM2321.
 *
 * @param[out] am2321_data Collect the data to this object from AM2321.
 *
 * @return Successed : 0, Failed : -1
 */
int measure(struct am2321 *am2321_data) {

  char i2c_dev_name[64], write_data[3];
  I2CSlave *am2321;

  sprintf(i2c_dev_name, I2C_DEV, 1);
  am2321 = gen_i2c_slave(i2c_dev_name, AM2321_DEV_NAME, AM2321_ID, 1, 3000);
  if (init_i2c_slave(am2321) == -1) {
    return -1;
  }

  // Step 1 : Wakeup AM2321.
  if (wakeup_am2321(am2321) == -1) {
    return -1;
  }

  // Step 2 : Write data to measuring temperature and humidity from sensor.
  if (write_mode_am2321(am2321) == -1) {
    return -1;
  }
  usleep(AM2321_WAIT_WRITEMODE);
  write_data[0] = 0x03; // Function code : 0x03 -> Read register of AM2321
                        //                 0x10 -> Write multiple data to register of AM2321
  write_data[1] = 0x00; // Top of address for read register of AM2321.
  write_data[2] = 0x04; // Size of data.
  if (write_i2c_slave(am2321, write_data, 3) == -1) {
    return -1;
  }

  // Step 3 : Recive data from AM2321.
  usleep(AM2321_WAIT_READMODE);
  if (read_i2c_slave(am2321, am2321_data->register_data, 8) == -1) {
    return -1;
  }

  if (term_i2c_slave(am2321) == -1) {
    return -1;
  }

  if (destroy_i2c_slave(am2321) == -1) {
    return -1;
  }

  if (check_err(am2321_data) == -1) {
    return -1;
  }

  if (check_crc(am2321_data) == -1) {
    return -1;
  }
  return 0;
}

#if MODULE
int init_module(void) {

  if ( register_chrdev(0x2321, "am2321_humidity", &am2321_humidity_fops ) ) {
    printk(KERN_INFO "am2321_humidity : louise chan ha genjitsu ja nai!?\n" );
    return -EBUSY;
  }
  if (register_chrdev(0x2322, "am2321_temperature", &am2321_temperature_fops ) ) {
    printk( KERN_INFO "am2321_temperature : louise chan ha genjitsu ja nai!?\n" );
    return -EBUSY;
  }
  cur_data = kmalloc(sizeof(struct am2321_data), GFP_KERNEL);

  return 0;
}

void cleanup_module(void){

  unregister_chrdev(0x2321, "am2321_humidity");
  unregister_chrdev(0x2322, "am2321_temperature");
  kfree(cur_data);
  printk(KERN_INFO "am2321 : Harukeginia no louise he todoke !!\n" );
}
#else

int measure_retry(struct am2321* am2321_data) {

  int count = 0;

  while (measure(am2321_data) == -1) {
    if (I2C_SLAVE_MAX_RETRY < ++count) {
      printk(KERN_WARNING "am2321 : Failed measure from am2321.\n");
      return -1;
    }
    printk(KERN_NOTICE "am2321 : Failed measure from am2321. retry %d of %d\n", count, I2C_SLAVE_MAX_RETRY);
    usleep(300000);
  }

  return 0;
}

void print_help(void) {

  printf("Usage: am2321 [OPTION]\n");
  printf("Receive the data from AM2321 which is I2C Slave device and print the value of temperature, humidity, discomfort index.\n\n");
  printf("  -c\tPrint the value in CSV format.\n");
  printf("  -j\tPrint the value in JSON format.\n");
  printf("  -r\tPrint the value in human readable format.\n");
  printf("  -h\tShow this message.\n\n");
  printf("Report bugs to mrkoh_t.bug-report@mem-notfound.net\n");
}

int main(int argc, char* argv[]) {

  int arg, format;
  double temp, hum, discomfort;
  struct am2321 am2321_data;

  while ((arg = getopt(argc, argv, "cjrh")) != -1) {
    format = arg;
  }
  if (format == 'h') {
    print_help();
    return 1;
  }
  measure_retry(&am2321_data);
  usleep(AM2321_WAIT_REFRESH);
  if (measure_retry(&am2321_data) == -1) {
    printf("Failed measure data from AM2321.\n");
  } else {
    temp = calc_temp(&am2321_data);
    hum = calc_hum(&am2321_data);
    discomfort = calc_discomfort(&am2321_data);

    switch(format) {
      case 'c':
        printf("%.1f,%.1f,%.1f\n"
          , temp
          , hum
          , discomfort
        );
        break;
      case 'j':
        printf("{\"Templature\":%.1f,\"Humidity\":%.1f,\"Discomfort\":%.1f}\n"
          , temp
          , hum
          , discomfort
        );
        break;
      case 'r':
      default:
        printf("Templature : %.1f\nHumidity   : %.1f\nDiscomfort : %.1f\n"
          , temp
          , hum
          , discomfort
        );
        break;
    }
  }

  return 0;
}
#endif

