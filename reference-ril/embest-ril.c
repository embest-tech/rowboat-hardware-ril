// Embest Tech co.Ltd
// tary, 18:00 2013-5-27
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <pthread.h>
#include <getopt.h>
#include <termios.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#define LOG_TAG "RILREF"
#include <utils/Log.h>
#include <cutils/properties.h>
#include <telephony/ril.h>

#define RIL_CardStatus RIL_CardStatus_v6
#define RIL_REQUEST_REGISTRATION_STATE RIL_REQUEST_VOICE_REGISTRATION_STATE
#define RIL_REQUEST_GPRS_REGISTRATION_STATE RIL_REQUEST_DATA_REGISTRATION_STATE
#define RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED

#define PPP_TTY_PATH "ppp0"

typedef enum {
	SIM_ABSENT = 0,
	SIM_NOT_READY = 1,
	SIM_READY = 2, /* SIM_READY means the radio state is RADIO_STATE_SIM_READY */
	SIM_PIN = 3,
	SIM_PUK = 4,
	SIM_NETWORK_PERSONALIZATION = 5
} SIM_Status;

typedef enum {
	HSPA_500HU = 0,
	ETCOM_W338,
	ETCOM_E300,
} e3g_module_t;

static int e3g_module_type = HSPA_500HU;
static char* e3g_module_name = "";
static int s_got_connect = 0;

#define IS_E300()	(e3g_module_type == ETCOM_E300)
#define IS_W338()	(e3g_module_type == ETCOM_W338)
#define IS_500HU()	(e3g_module_type == HSPA_500HU)

static int ppp_options_write(const char* mod_name,
				const char* apn,
				const char* username,
				const char* password);

static void onRequest (int request, void *data, size_t datalen, RIL_Token t);
static RIL_RadioState currentState();
static int onSupports (int requestCode);
static void onCancel (RIL_Token t);
static const char *getVersion();

extern const char * requestToString(int request);

/*** Static Variables ***/
static const RIL_RadioFunctions s_callbacks = {
	RIL_VERSION,
	onRequest,
	currentState,
	onSupports,
	onCancel,
	getVersion
};

#ifdef RIL_SHLIB
static const struct RIL_Env *s_rilenv;

#define RIL_onRequestComplete(t, e, response, responselen) s_rilenv->OnRequestComplete(t,e, response, responselen)
#define RIL_onUnsolicitedResponse(a,b,c) s_rilenv->OnUnsolicitedResponse(a,b,c)
#define RIL_requestTimedCallback(a,b,c) s_rilenv->RequestTimedCallback(a,b,c)
#else
#error define RIL_SHLIB!!!
#endif

static const char* s_device_path = NULL;

static RIL_RadioState sState = RADIO_STATE_UNAVAILABLE;

static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_state_cond = PTHREAD_COND_INITIALIZER;
/* trigger change to this with s_state_cond */
static int s_closed = 0;

static const struct timeval TIMEVAL_SIMPOLL = {1,0};
static const struct timeval TIMEVAL_0 = {0,0};

pthread_t s_tid_mainloop;


static int isRadioOn();
static void setRadioState(RIL_RadioState newState);

static void usage(char *s)
{
	fprintf(stderr, "reference-ril requires: -p <tcp port> or -d /dev/tty_device\n");
}

/** Returns SIM_NOT_READY on error */
static SIM_Status getSIMStatus()
{
	ATResponse *p_response = NULL;
	int err;
	int ret;
	char *cpinLine;
	char *cpinResult;

	if (sState == RADIO_STATE_OFF || sState == RADIO_STATE_UNAVAILABLE) {
		ret = SIM_NOT_READY;
		goto done;
	}

	err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);

	if (err != 0) {
		ret = SIM_NOT_READY;
		goto done;
	}

	switch (at_get_cme_error(p_response)) {
		case CME_SUCCESS:
			break;

		case CME_SIM_NOT_INSERTED:
			ret = SIM_ABSENT;
			goto done;

		default:
			ret = SIM_NOT_READY;
			goto done;
	}

	/* CPIN? has succeeded, now look at the result */

	cpinLine = p_response->p_intermediates->line;
	err = at_tok_start (&cpinLine);

	if (err < 0) {
		ret = SIM_NOT_READY;
		goto done;
	}

	err = at_tok_nextstr(&cpinLine, &cpinResult);

	if (err < 0) {
		ret = SIM_NOT_READY;
		goto done;
	}

	if (0 == strcmp (cpinResult, "SIM PIN")) {
		ret = SIM_PIN;
		goto done;
	} else if (0 == strcmp (cpinResult, "SIM PUK")) {
		ret = SIM_PUK;
		goto done;
	} else if (0 == strcmp (cpinResult, "PH-NET PIN")) {
		return SIM_NETWORK_PERSONALIZATION;
	} else if (0 != strcmp (cpinResult, "READY"))  {
		/* we're treating unsupported lock types as "sim absent" */
		ret = SIM_ABSENT;
		goto done;
	}

	at_response_free(p_response);
	p_response = NULL;
	cpinResult = NULL;

	ret = SIM_READY;

done:
	at_response_free(p_response);
	return ret;
}

/**
 * Get the current card status.
 *
 * This must be freed using freeCardStatus.
 * @return: On success returns RIL_E_SUCCESS
 */
static int getCardStatus(RIL_CardStatus **pp_card_status) {
	static RIL_AppStatus app_status_array[] = {
		// SIM_ABSENT = 0
		{ RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
		  NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
		// SIM_NOT_READY = 1
		{ RIL_APPTYPE_SIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
		  NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
		// SIM_READY = 2
		{ RIL_APPTYPE_SIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
		  NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
		// SIM_PIN = 3
		{ RIL_APPTYPE_SIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
		  NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
		// SIM_PUK = 4
		{ RIL_APPTYPE_SIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
		  NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
		// SIM_NETWORK_PERSONALIZATION = 5
		{ RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
		  NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN }
	};
	RIL_CardState card_state;
	int num_apps;

	int sim_status = getSIMStatus();
	if (sim_status == SIM_ABSENT) {
		card_state = RIL_CARDSTATE_ABSENT;
		num_apps = 0;
	} else {
		card_state = RIL_CARDSTATE_PRESENT;
		num_apps = 1;
	}

	// Allocate and initialize base card status.
	RIL_CardStatus *p_card_status = malloc(sizeof(RIL_CardStatus));
	p_card_status->card_state = card_state;
	p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
	p_card_status->gsm_umts_subscription_app_index = RIL_CARD_MAX_APPS;
	p_card_status->cdma_subscription_app_index = RIL_CARD_MAX_APPS;
	p_card_status->num_applications = num_apps;

	// Initialize application status
	int i;
	for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
		p_card_status->applications[i] = app_status_array[SIM_ABSENT];
	}

	// Pickup the appropriate application status
	// that reflects sim_status for gsm.
	if (num_apps != 0) {
		// Only support one app, gsm
		p_card_status->num_applications = 1;
		p_card_status->gsm_umts_subscription_app_index = 0;

		// Get the correct app status
		p_card_status->applications[0] = app_status_array[sim_status];
	}

	*pp_card_status = p_card_status;
	return RIL_E_SUCCESS;
}

/**
 * Free the card status returned by getCardStatus
 */
static void freeCardStatus(RIL_CardStatus *p_card_status) {
	free(p_card_status);
}

/**
 * SIM ready means any commands that access the SIM will work, including:
 *  AT+CPIN, AT+CSMS, AT+CNMI, AT+CRSM
 *  (all SMS-related commands)
 */

static void pollSIMState (void *param)
{
	ATResponse *p_response;
	int ret;

	if (sState != RADIO_STATE_SIM_NOT_READY) {
		// no longer valid to poll
		return;
	}

	switch(getSIMStatus()) {
		case SIM_ABSENT:
		case SIM_PIN:
		case SIM_PUK:
		case SIM_NETWORK_PERSONALIZATION:
		default:
			setRadioState(RADIO_STATE_SIM_LOCKED_OR_ABSENT);
			return;

		case SIM_NOT_READY:
			RIL_requestTimedCallback (pollSIMState, NULL, &TIMEVAL_SIMPOLL);
			return;

		case SIM_READY:
			setRadioState(RADIO_STATE_SIM_READY);
			return;
	}
}

/** do post- SIM ready initialization */
static void onSIMReady()
{
	return;
}

/** do post-AT+CFUN=1 initialization */
static void onRadioPowerOn()
{
	pollSIMState(NULL);
}

static void requestRadioPower(void *data, size_t datalen, RIL_Token t)
{
	int onOff;

	int err;
	ATResponse *p_response = NULL;

	assert (datalen >= sizeof(int *));
	onOff = ((int *)data)[0];

	if (onOff == 0 && sState != RADIO_STATE_OFF) {
		err = at_send_command("AT+CFUN=0", &p_response);
		if (err < 0 || p_response->success == 0) goto error;
		setRadioState(RADIO_STATE_OFF);
	} else if (onOff > 0 && sState == RADIO_STATE_OFF) {
		err = at_send_command("AT+CFUN=1", &p_response);
		if (err < 0|| p_response->success == 0) {
			// Some stacks return an error when there is no SIM,
			// but they really turn the RF portion on
			// So, if we get an error, let's check to see if it
			// turned on anyway

			if (isRadioOn() != 1) {
				goto error;
			}
		}
		setRadioState(RADIO_STATE_SIM_NOT_READY);
	}

	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;
error:
	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSignalStrength(void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response = NULL;
	int err;
	int response[2];
	char *line;
	RIL_SignalStrength_v6 sig_strength[1];

	err = at_send_command_singleline("AT+CSQ", "+CSQ:", &p_response);

	if (err < 0 || p_response->success == 0) {
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
		goto error;
	}

	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &(response[0]));
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &(response[1]));
	if (err < 0) goto error;

	sig_strength->GW_SignalStrength.signalStrength = response[0];
	sig_strength->GW_SignalStrength.bitErrorRate = response[1];
	RIL_onRequestComplete(t, RIL_E_SUCCESS, sig_strength, sizeof(RIL_SignalStrength_v5));

	at_response_free(p_response);
	return;

error:
	LOGE("requestSignalStrength must never return an error when radio is on");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	at_response_free(p_response);
}

static void requestOperator(void *data, size_t datalen, RIL_Token t)
{
	char *response[3];

	memset(response, 0, sizeof(response));

	response[0] = "CDMA";

	RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));

	return;
error:
	LOGE("requestOperator must not return error when radio is on");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestRegistrationState(int request, void *data,
										size_t datalen, RIL_Token t)
{
	char * responseStr[4];
	int count = 3;

	asprintf(&responseStr[0], "%d", 1);
	asprintf(&responseStr[1], "%x", 0);
	asprintf(&responseStr[2], "%x", 0);

	RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, count * sizeof(char*));

	return;
error:
	LOGE("requestRegistrationState must never return an error when radio is on");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestQueryNetworkSelectionMode(
				void *data, size_t datalen, RIL_Token t)
{
	int response = 0;

	// 0	auto selection
	// 1	manual selection
	response = 1;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
	return;
error:
	LOGE("requestQueryNetworkSelectionMode must never return error when radio is on");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSetupDataCall(void *data, size_t datalen, RIL_Token t)
{
	int err;
	char *response[2] = { "1", PPP_TTY_PATH };
	const char* apn = ((const char **)data)[2];
	const char* username = ((const char **)data)[3];
	const char* password = ((const char **)data)[4];
	int retry = 10;
	int timeout = 0;
	char prop_buf[20];
	RIL_Data_Call_Response_v6 ril_dc_r[1];

	LOGD("~~~t~~~requestSetupDataCall()");
	LOGD(" APN '%s', USER '%s', PASS '%s'", apn, username, password);
	LOGD(" tech = %s  DataProfile = %s", ((const char **)data)[0], ((const char **)data)[1]);

	ppp_options_write(e3g_module_name, apn, username, password);

	s_got_connect = 0;

	if(0 != property_set("ctl.stop", "ril-pppd")) {
	}

	sleep(1);

	//启动pppd服务
	LOGD("~~~t~~~starting service ril-pppd");
	if(0 != property_set("ctl.start", "ril-pppd")) {
		LOGD(" failed\n");
		goto error;
	}

	//等待链接建立好
	do {
		if (s_got_connect) {
			sleep(1);
			break;
		}
		sleep(1);
	} while (timeout++ <= 10);

	while (timeout++ < 15) {
		if(0 != property_get("net.dns1", prop_buf, NULL)
		&& strlen(prop_buf) >= 7) {
			break;
		}
		sleep(1);
	}

	ril_dc_r->status = 0;
	ril_dc_r->suggestedRetryTime = 0;
	ril_dc_r->cid = 1;
	ril_dc_r->active = 2;
	ril_dc_r->type = "PPP";
	ril_dc_r->ifname = "ppp0";

	if (0 == property_get("net.ppp0.local-ip", prop_buf, NULL) || (strlen(prop_buf) < 7)){
		goto error;
	}
	ril_dc_r->addresses = strdup(prop_buf);

	if (0 == property_get("net.dns1", prop_buf, NULL) || (strlen(prop_buf) < 7)){
		goto error;
	}
	ril_dc_r->dnses = strdup(prop_buf);

	if (0 == property_get("net.ppp0.remote-ip", prop_buf, NULL) || (strlen(prop_buf) < 7)){
		goto error;
	}
	ril_dc_r->gateways = strdup(prop_buf);
	
	RIL_onRequestComplete(t, RIL_E_SUCCESS, ril_dc_r, sizeof(ril_dc_r));
	return;
error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestDeactivateDataCall(void *data, size_t datalen, RIL_Token t)
{
	int timeout = 0;

	LOGD("~~~t~~~requestDeactivateDataCall()");
	LOGD("~~~t~~~stoping service ril-pppd");
	if(0 != property_set("ctl.stop", "ril-pppd")) {
		LOGD(" failed\n");
		goto error;
	}

	//等待链接释放
	do {
		sleep(3);
		timeout += 3;
		if(access("/sys/class/net/ppp0/operstate", F_OK) != 0) {
			break;
		}
	} while (timeout <= 18);

	if(access("/sys/class/net/ppp0/operstate", F_OK) == 0){
		goto error;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;
error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/*** Callback methods from the RIL library to us ***/

/**
 * Call from RIL to us to make a RIL_REQUEST
 *
 * Must be completed with a call to RIL_onRequestComplete()
 *
 * RIL_onRequestComplete() may be called from any thread, before or after
 * this function returns.
 *
 * Will always be called from the same thread, so returning here implies
 * that the radio is ready to process another command (whether or not
 * the previous command has completed).
 */
static void
onRequest (int request, void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response;
	int err;

	/* Ignore all requests except RIL_REQUEST_GET_SIM_STATUS
	 * when RADIO_STATE_UNAVAILABLE.
	 */
	if (sState == RADIO_STATE_UNAVAILABLE
	&& request != RIL_REQUEST_GET_SIM_STATUS
	) {
		RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
		return;
	}

	/* Ignore all non-power requests when RADIO_STATE_OFF
	 * (except RIL_REQUEST_GET_SIM_STATUS)
	 */
	if (sState == RADIO_STATE_OFF
	&& !(request == RIL_REQUEST_RADIO_POWER
	|| request == RIL_REQUEST_GET_SIM_STATUS)
	) {
		RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
		return;
	}

	LOGD("~~~t~~~ onRequest: %s", requestToString(request));

	switch (request) {
	case RIL_REQUEST_GET_SIM_STATUS: {
		RIL_CardStatus *p_card_status;
		char *p_buffer;
		int buffer_size;

		int result = getCardStatus(&p_card_status);
		if (result == RIL_E_SUCCESS) {
			p_buffer = (char *)p_card_status;
			buffer_size = sizeof(*p_card_status);
		} else {
			p_buffer = NULL;
			buffer_size = 0;
		}
		RIL_onRequestComplete(t, result, p_buffer, buffer_size);
		freeCardStatus(p_card_status);
		break;
	}
	case RIL_REQUEST_RADIO_POWER:
		requestRadioPower(data, datalen, t);
		break;
	case RIL_REQUEST_GET_CURRENT_CALLS:
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
		break;

	/* ###################### essential request start ###################### */

	case RIL_REQUEST_SIGNAL_STRENGTH:
		requestSignalStrength(data, datalen, t);
		break;

	case RIL_REQUEST_GET_IMSI:
		if (IS_E300()) {
			RIL_onRequestComplete(t, RIL_E_SUCCESS, "460030237471781", sizeof(char *));
		} else {
			RIL_onRequestComplete(t, RIL_E_SUCCESS, "460010237471781", sizeof(char *));
		}
		
		break;

	case RIL_REQUEST_OPERATOR:
		requestOperator(data, datalen, t);
		break;

	case RIL_REQUEST_REGISTRATION_STATE:
	case RIL_REQUEST_GPRS_REGISTRATION_STATE:
		requestRegistrationState(request, data, datalen, t);
		break;

	case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
		requestQueryNetworkSelectionMode(data, datalen, t);
		break;


	case RIL_REQUEST_SETUP_DATA_CALL:
		requestSetupDataCall(data, datalen, t);
		break;

	case RIL_REQUEST_DEACTIVATE_DATA_CALL:
		requestDeactivateDataCall(data, datalen, t);
		break;

	/* ###################### essential request end   ###################### */
	default:
		RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
		break;
	}
}

/**
 * Synchronous call from the RIL to us to return current radio state.
 * RADIO_STATE_UNAVAILABLE should be the initial state.
 */
static RIL_RadioState
currentState()
{
	return sState;
}
/**
 * Call from RIL to us to find out whether a specific request code
 * is supported by this implementation.
 *
 * Return 1 for "supported" and 0 for "unsupported"
 */

static int
onSupports (int request)
{
	//@@@ todo
	LOGD("~~~t~~~ onSupports: %s", requestToString(request));
	return 1;
}

static void onCancel (RIL_Token t)
{
	//@@@todo

}

static const char * getVersion(void)
{
	return "android reference-ril 1.0";
}

static void
setRadioState(RIL_RadioState newState)
{
	RIL_RadioState oldState;

	pthread_mutex_lock(&s_state_mutex);

	oldState = sState;

	if (s_closed > 0) {
		// If we're closed, the only reasonable state is
		// RADIO_STATE_UNAVAILABLE
		// This is here because things on the main thread
		// may attempt to change the radio state after the closed
		// event happened in another thread
		newState = RADIO_STATE_UNAVAILABLE;
	}

	if (sState != newState || s_closed > 0) {
		sState = newState;

		pthread_cond_broadcast (&s_state_cond);
	}

	pthread_mutex_unlock(&s_state_mutex);


	/* do these outside of the mutex */
	if (sState != oldState) {
		RIL_onUnsolicitedResponse (RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
									NULL, 0);

		/* FIXME onSimReady() and onRadioPowerOn() cannot be called
		 * from the AT reader thread
		 * Currently, this doesn't happen, but if that changes then these
		 * will need to be dispatched on the request thread
		 */
		if (sState == RADIO_STATE_SIM_READY) {
			onSIMReady();
		} else if (sState == RADIO_STATE_SIM_NOT_READY) {
			onRadioPowerOn();
		}
	}
}


/** returns 1 if on, 0 if off, and -1 on error */
static int isRadioOn()
{
	ATResponse *p_response = NULL;
	int err;
	char *line;
	char ret;

	err = at_send_command_singleline("AT+CFUN?", "+CFUN:", &p_response);

	if (err < 0 || p_response->success == 0) {
		// assume radio is off
		goto error;
	}

	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextbool(&line, &ret);
	if (err < 0) goto error;

	at_response_free(p_response);

	return (int)ret;

error:

	at_response_free(p_response);
	return -1;
}

/**
 * Initialize everything that can be configured while we're still in
 * AT+CFUN=0
 */
static void initializeCallback(void *param)
{
	ATResponse *p_response = NULL;
	int err;

	setRadioState (RADIO_STATE_OFF);

	at_handshake();

	/* note: we don't check errors here. Everything important will
	   be handled in onATTimeout and onATReaderClosed */

	/*  atchannel is tolerant of echo but it must */
	/*  have verbose result codes */

	/* assume radio is off on error */
	if (isRadioOn() > 0) {
		setRadioState (RADIO_STATE_SIM_NOT_READY);
	}
}

static void waitForClose()
{
	pthread_mutex_lock(&s_state_mutex);

	while (s_closed == 0) {
		pthread_cond_wait(&s_state_cond, &s_state_mutex);
	}

	pthread_mutex_unlock(&s_state_mutex);
}

/**
 * Called by atchannel when an unsolicited line appears
 * This is called on atchannel's reader thread. AT commands may
 * not be issued here
 */
static void onUnsolicited (const char *s, const char *sms_pdu)
{
	char *line = NULL;
	int err;

	/* Ignore unsolicited responses until we're initialized.
	 * This is OK because the RIL library will poll for initial state
	 */
	if (sState == RADIO_STATE_UNAVAILABLE) {
		return;
	}

	if (strStartsWith(s,"+CREG:")
	|| strStartsWith(s,"+CGREG:")
	//|| strStartsWith(s, "CONNECT")
	) {
		RIL_onUnsolicitedResponse (
			RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED,
			NULL, 0
		);
	} else if (strStartsWith(s, "CONNECT")) {
		s_got_connect = 1;
	}
}

/* Called on command thread */
static void onATTimeout()
{
	LOGI("AT channel timeout; closing\n");
	at_close();

	s_closed = 1;

	/* FIXME cause a radio reset here */

	setRadioState (RADIO_STATE_UNAVAILABLE);
}


/* Called on command or reader thread */
static void onATReaderClosed()
{
	LOGI("AT channel closed\n");
	at_close();
	s_closed = 1;

	setRadioState (RADIO_STATE_UNAVAILABLE);
}

int ppp_options_conf(const char* module, const char** env);

static int ppp_options_write(	const char* mod_name,
				const char* apn,
				const char* username,
				const char* password
) {
	char lines[3][PROPERTY_VALUE_MAX];
	const char* env[4] = {NULL};
	int i, r;

	sprintf(lines[0], "apn=\"%s\"", apn);
	sprintf(lines[1], "username=\"%s\"", username);
	sprintf(lines[2], "password=\"%s\"", password);

	for (i = 0; i < 3; i++) {
		env[i] = lines[i];
	}

	r = ppp_options_conf(mod_name, env);

	return r;
}

/*
#[3gsupport,HSPA-500HU,    setprop ro.radio.3g.name HSPA-500HU]
#[3gsupport,ETCOM-W338,    setprop ro.radio.3g.name ETCOM-W338]
#[3gsupport,ETCOM-E300,    setprop ro.radio.3g.name ETCOM-E300]
*/
static int get_module_type(void) {
	int mod_type;
	static char mod_name[PROPERTY_VALUE_MAX];

	if (0 == property_get("ro.radio.3g.name", mod_name, NULL)) {
		return -1;
	}

	mod_type = HSPA_500HU;

	if (strcmp(mod_name, "HSPA-500HU") == 0) {
		mod_type = HSPA_500HU;
	} else if (strcmp(mod_name, "ETCOM-W338") == 0) {
		mod_type = ETCOM_W338;
	} else if (strcmp(mod_name, "ETCOM-W388") == 0) {
		mod_type = HSPA_500HU;
	} else if (strcmp(mod_name, "ETCOM-E300") == 0) {
		mod_type = ETCOM_E300;
	}
	e3g_module_type	= mod_type;
	e3g_module_name = mod_name;

	LOGD("e3g_module_type = %d", mod_type);
	LOGD("e3g_module_name = %s", mod_name);
	return 0;
}

static void set_usbtty_attr(int fd)
{
	struct termios  ios;

	if (isatty(fd) == 0) {
		return;
	}

	/* disable echo on serial ports */
	tcgetattr( fd, &ios );
	ios.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
	ios.c_cflag |= CS8;

	cfsetispeed(&ios, B115200);
	cfsetospeed(&ios, B115200);

	ios.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);

	tcsetattr( fd, TCSANOW, &ios );
}

static void *
mainLoop(void *param)
{
	int fd;
	int ret;

	AT_DUMP("== ", "entering mainLoop()", -1 );
	at_set_on_reader_closed(onATReaderClosed);
	at_set_on_timeout(onATTimeout);

	for (;;) {
		fd = -1;
		while  (fd < 0) {
			if (s_device_path != NULL) {
				fd = open (s_device_path, O_RDWR);
			}

			if (fd < 0) {
				perror ("opening AT interface. retrying...");
				sleep(10);
				/* never returns */
			}
		}

		set_usbtty_attr(fd);

		s_closed = 0;
		ret = at_open(fd, onUnsolicited);

		if (ret < 0) {
			LOGE ("AT error %d on at_open\n", ret);
			return 0;
		}

		RIL_requestTimedCallback(initializeCallback, NULL, &TIMEVAL_0);

		// Give initializeCallback a chance to dispatched, since
		// we don't presently have a cancellation mechanism
		sleep(1);

		waitForClose();
		LOGI("Re-opening after close");
	}
}

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv)
{
	int ret;
	int fd = -1;
	int opt;
	pthread_attr_t attr;

	s_rilenv = env;

	while ( -1 != (opt = getopt(argc, argv, "d:"))) {
		switch (opt) {
		case 'd':
			s_device_path = optarg;
			LOGI("Opening tty device %s\n", s_device_path);
			break;

		default:
			usage(argv[0]);
			return NULL;
		}
	}

	if (s_device_path == NULL) {
		usage(argv[0]);
		return NULL;
	}

	// tary 17:34 2010-12-1
	get_module_type();

	pthread_attr_init (&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	ret = pthread_create(&s_tid_mainloop, &attr, mainLoop, NULL);

	return &s_callbacks;
}
