#ifndef CLOUD_H__
#define CLOUD_H__

#include <zephyr.h>
#include <stdbool.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TEMPERATURE_THRESHOLD_CELSIUS CONFIG_TEMPERATURE_THRESHOLD_CENTICELSIUS / (float)10

struct current_state {
};

struct desired_state {
};

struct track_reported {
	bool publishVersion; // Simple one time check for reported version
	double inside;
	double outside;
};

int cloud_decode_response(char *input, struct desired_state *cfg);
int cloud_report_state(
	struct current_state *currentState, 
	struct track_reported *trackReported,
	bool publishFullUpdate);

#ifdef __cplusplus
}
#endif
#endif
