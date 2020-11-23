#ifndef _BLE_H_
#define _BLE_H_

#define NAME_LEN 30

struct sensor_data {
	float temperature;
	char name[NAME_LEN];
	bool fresh;
};

extern struct sensor_data inside;
extern struct sensor_data outside;

void beacons_init();

#endif /* _BLE_H_ */