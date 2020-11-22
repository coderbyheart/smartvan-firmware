#ifndef CLOUD_H__
#define CLOUD_H__

#include <zephyr.h>
#include <stdbool.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

struct current_state {
};

struct desired_state {
};

struct track_reported {
	bool publishVersion; // Simple one time check for reported version
};

struct track_desired {
	bool overrideThreshold; // Simple switch whether to override the desired property
};

int cloud_decode_response(char *input, struct desired_state *cfg);
int cloud_report_state(
	struct current_state *currentState, 
	struct track_reported *trackReported,
	struct track_desired *trackDesired);

#ifdef __cplusplus
}
#endif
#endif
