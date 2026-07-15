#ifndef _CONN_NOTIFY_H
#define _CONN_NOTIFY_H

#include "Sonde.h"
#include "conn.h"

class ConnNotify : public Conn {
public:
	void init();
	void netsetup();
	void netshutdown();
	void updateSonde( SondeInfo *si );
	void updateStation( PosInfo *pi );
	bool replayReady();
	String getStatus();
	String getName();

private:
	// Per-channel latch state (slot = si - sonde.sondeList, 0..MAXSONDE).
	char  lastSerial[MAXSONDE+1][12];
	bool  alerted[MAXSONDE+1];
	float peakAlt[MAXSONDE+1];
	unsigned long lastTry[MAXSONDE+1];   // millis() of last send attempt (retry backoff)
	uint8_t lastTries[MAXSONDE+1];       // failed attempts for the current sonde (give-up cap)
	bool  initialized = false;
	char  laststatus[48] = "idle";
};

extern ConnNotify connNotify;

#endif
