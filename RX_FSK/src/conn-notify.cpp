#include "../features.h"
#if FEATURE_NOTIFY

#include "conn-notify.h"
#include "posinfo.h"
#include "logger.h"
#include <WiFiClient.h>

extern const char *sondeTypeStr[];
extern float calcLatLonDist(float lat1, float lon1, float lat2, float lon2);
extern boolean connected;

#define NOTIFY_DESCENT_VS (-1.0f)   // m/s; below this counts as descending
#define NOTIFY_ALT_DROP_M (300.0f)  // m dropped from peak also counts as descending
#define NOTIFY_TIMEOUT_MS 5000
#define NOTIFY_RETRY_MS 60000       // min gap between send attempts after a failure
#define NOTIFY_MAX_TRIES 5          // give up (latch) after this many failed attempts

// Parse url into (secure, host, port, path). Returns false on malformed input.
static bool parseUrl(const char *url, bool &secure, char *host, int hostlen, int &port, char *path, int pathlen) {
	if (!url) return false;
	const char *p = url;
	if (strncmp(p, "https://", 8) == 0) { secure = true;  port = 443; p += 8; }
	else if (strncmp(p, "http://", 7) == 0) { secure = false; port = 80; p += 7; }
	else return false;
	const char *slash = strchr(p, '/');
	const char *colon = strchr(p, ':');
	const char *hostend = slash;
	if (colon && (!slash || colon < slash)) hostend = colon;
	if (!hostend) hostend = p + strlen(p);
	int hl = (int)(hostend - p);
	if (hl <= 0 || hl >= hostlen) return false;
	memcpy(host, p, hl); host[hl] = 0;
	if (colon && (!slash || colon < slash)) {
		port = atoi(colon + 1);
		if (port <= 0) return false;
	}
	if (slash) strlcpy(path, slash, pathlen);
	else       strlcpy(path, "/", pathlen);
	return true;
}

void ConnNotify::init() {
	memset(lastSerial, 0, sizeof(lastSerial));
	memset(alerted, 0, sizeof(alerted));
	memset(lastTry, 0, sizeof(lastTry));
	memset(lastTries, 0, sizeof(lastTries));
	for (int i = 0; i <= MAXSONDE; i++) peakAlt[i] = -100000.0f;
	initialized = true;
}

void ConnNotify::netsetup() {}
void ConnNotify::netshutdown() {}
void ConnNotify::updateStation(PosInfo *pi) {}

// Cache-drain dispatch (drainConnectors) only delivers frames to connectors whose
// replayReady() is true; mirror the other network connectors so we actually run.
bool ConnNotify::replayReady() {
	return sonde.config.notify.active && sonde.config.notify.topic[0] && connected;
}

void ConnNotify::updateSonde(SondeInfo *si) {
	if (!sonde.config.notify.active) return;
	if (sonde.config.notify.topic[0] == 0) return;
	if (!initialized) init();

	// Replayed cache frames arrive via a stack temp (not a sondeList element), so the
	// slot falls outside [0,MAXSONDE] and is skipped here on purpose: we only alert on
	// the live frame, never on backfilled history.
	int slot = (int)(si - sonde.sondeList);
	if (slot < 0 || slot > MAXSONDE) return;

	// Identity: ser and id are always written together by the decoders (RS41 sets both
	// to the same serial; DFM/M10 derive ser from id), so this never flips between two
	// different values -- only empty<->serial. Skip unidentified frames (e.g. a DFM
	// bad-frame clear zeroes ser/id) so an empty key can't overwrite the stored serial
	// and spuriously reset the latch/peak.
	const char *ser = (si->d.ser[0]) ? si->d.ser : si->d.id;
	if (ser[0] == 0) return;
	// New (different, identified) sonde in this slot? reset all per-sonde state, including
	// the backoff timer/try-count -- otherwise a fresh sonde would inherit the previous
	// sonde's backoff and have its first alert suppressed for up to NOTIFY_RETRY_MS.
	if (strncmp(lastSerial[slot], ser, sizeof(lastSerial[slot])) != 0) {
		strlcpy(lastSerial[slot], ser, sizeof(lastSerial[slot]));
		alerted[slot] = false;
		peakAlt[slot] = -100000.0f;
		lastTry[slot] = 0;
		lastTries[slot] = 0;
	}

	// Need a fresh, complete fix. VALIDPOS only covers lat/lon; altitude and vertical
	// speed have their own bits (DFM sets them independently, e.g. clears VALIDALT when
	// alt==0). Require valid altitude, and skip stale ("old", 0x80) positions, so we never
	// evaluate "landing" against a bogus alt=0 or a reused last-known fix.
	if (!VALIDPOS(si->d.validPos)) return;
	if (!VALIDALT(si->d.validPos)) return;
	if (si->d.validPos & 0x80) return;

	if (si->d.alt > peakAlt[slot]) peakAlt[slot] = si->d.alt;

	// Station position: GPS if valid, else fixed rxlat/rxlon.
	float mylat = sonde.config.rxlat, mylon = sonde.config.rxlon;
	bool valid = !(isnan(mylat) || isnan(mylon));
	if (gpsPos.valid) { mylat = gpsPos.lat; mylon = gpsPos.lon; valid = true; }
	if (!valid) return;

	float distkm = calcLatLonDist(mylat, mylon, si->d.lat, si->d.lon) / 1000.0f;
	bool near = distkm <= (float)sonde.config.notify.dist;
	// Trust vs only when its validity bit is set; the altitude drop from peak is the
	// fallback descent signal when vs is missing/unreliable.
	bool descending = (VALIDVS(si->d.validPos) && si->d.vs < NOTIFY_DESCENT_VS) ||
	                  ((peakAlt[slot] - si->d.alt) > NOTIFY_ALT_DROP_M);
	bool low = si->d.alt < (float)sonde.config.notify.alt * 1000.0f;

	if (!(near && descending && low)) return;
	if (alerted[slot]) return;

	// Cross-channel dedup: if this serial was already alerted in another slot (e.g. the
	// same frequency configured on two channels), don't alert again for it.
	for (int j = 0; j <= MAXSONDE; j++) {
		if (j != slot && alerted[j] && strncmp(lastSerial[j], ser, sizeof(lastSerial[j])) == 0) {
			alerted[slot] = true;
			return;
		}
	}

	// Backoff: at most one send attempt per NOTIFY_RETRY_MS. Otherwise a persistently
	// unreachable/erroring server would re-run a blocking connect+POST on every frame
	// (~1/s) while a qualifying sonde is in view, stalling the UI/web loop.
	unsigned long nowms = millis();
	if (lastTry[slot] != 0 && (nowms - lastTry[slot]) < NOTIFY_RETRY_MS) return;
	lastTry[slot] = nowms;

	// Parse server URL and send. Outcomes:
	//   ok        -> delivered; latch (one alert per sonde).
	//   permanent -> a failure retrying can't fix (bad/https config, or HTTP 4xx except
	//                429); counts toward NOTIFY_MAX_TRIES, then we give up.
	//   transient -> connect failure / 5xx / 429 / timeout; does NOT count -- keep retrying
	//                on the backoff (bounded anyway by the sonde leaving view), so a WiFi
	//                outage during descent doesn't permanently burn the alert.
	bool ok = false, permanent = false;
	bool secure; char host[96]; int port; char basepath[64];
	if (!parseUrl(sonde.config.notify.server, secure, host, sizeof(host), port, basepath, sizeof(basepath))) {
		strlcpy(laststatus, "bad server url", sizeof(laststatus));
		permanent = true;
	} else if (secure) {
		strlcpy(laststatus, "https unsupported (no TLS)", sizeof(laststatus));
		LOG_W("notify", "https server '%s' not supported (no TLS in firmware); use http://\n",
		      sonde.config.notify.server);
		permanent = true;
	} else {
		// Compose full path: basepath (usually "/") + topic, avoiding a double slash.
		char path[128];
		if (basepath[strlen(basepath)-1] == '/')
			snprintf(path, sizeof(path), "%s%s", basepath, sonde.config.notify.topic);
		else
			snprintf(path, sizeof(path), "%s/%s", basepath, sonde.config.notify.topic);

		// Build body + title. Only include the vertical rate when it's actually valid
		// (descent may have been detected via the altitude-drop fallback, with vs unset);
		// printing an invalid vs would show a garbage "descending N m/s".
		uint8_t rt = si->type;
		char vsinfo[32];
		if (VALIDVS(si->d.validPos))
			snprintf(vsinfo, sizeof(vsinfo), ", %s %.1f m/s",
			         (si->d.vs < 0 ? "descending" : "climbing"),
			         (si->d.vs < 0 ? -si->d.vs : si->d.vs));
		else
			vsinfo[0] = 0;
		char title[80], body[256];
		snprintf(title, sizeof(title), "Sonde landing near: %s", ser);
		snprintf(body, sizeof(body),
		         "%s %s\n%.1f km away, alt %d m%s\nlat %.4f, lon %.4f",
		         (rt < NSondeTypes ? sondeTypeStr[rt] : "?"), ser,
		         distkm, (int)si->d.alt, vsinfo,
		         si->d.lat, si->d.lon);

		WiFiClient cl;
		// WiFiClient(=NetworkClient) does NOT override setTimeout, so this hits
		// Stream::setTimeout, whose unit is MILLISECONDS (it bounds readStringUntil below).
		// Passing seconds here would give a ~5 ms read window -> the status read times out ->
		// ok stays false -> the alert never latches -> the (already-delivered) push re-fires
		// every frame. Must be milliseconds.
		cl.setTimeout(NOTIFY_TIMEOUT_MS);

		if (cl.connect(host, port)) {
			char clickurl[80];
			snprintf(clickurl, sizeof(clickurl), "https://sondehub.org/%s", ser);
			cl.printf("POST %s HTTP/1.1\r\n", path);
			if (port == 80) cl.printf("Host: %s\r\n", host);
			else            cl.printf("Host: %s:%d\r\n", host, port);   // vhost/proxy needs the port
			cl.print("Title: "); cl.print(title); cl.print("\r\n");
			cl.print("Tags: balloon,warning\r\n");
			cl.print("Priority: 4\r\n");
			cl.print("Click: "); cl.print(clickurl); cl.print("\r\n");
			if (sonde.config.notify.token[0])
				cl.printf("Authorization: Bearer %s\r\n", sonde.config.notify.token);
			cl.printf("Content-Length: %d\r\n", (int)strlen(body));
			cl.print("Connection: close\r\n\r\n");
			cl.print(body);
			// Read status line (best-effort, bounded by timeout).
			String status = cl.readStringUntil('\n');
			cl.stop();
			// "HTTP/1.x CODE ..." -> extract CODE. 2xx = delivered; 4xx (except 429
			// rate-limit) is a permanent client/config error; anything else is transient.
			int code = 0, sp = status.indexOf(' ');
			if (sp > 0) code = status.substring(sp + 1).toInt();
			ok = (code >= 200 && code < 300);
			if (!ok && code >= 400 && code < 500 && code != 429) permanent = true;
			if (ok) snprintf(laststatus, sizeof(laststatus), "sent %s", ser);
			else    snprintf(laststatus, sizeof(laststatus), "http %d %s", code, ser);
		} else {
			strlcpy(laststatus, "connect failed", sizeof(laststatus));   // transient
		}
	}

	if (ok) {
		alerted[slot] = true;   // delivered: one alert per sonde
	} else if (permanent) {
		if (lastTries[slot] < 255) lastTries[slot]++;
		if (lastTries[slot] >= NOTIFY_MAX_TRIES) {
			alerted[slot] = true;   // give up on an unfixable failure rather than retry forever
			LOG_W("notify", "giving up on %s after %d permanent failures (%s)\n",
			      ser, (int)lastTries[slot], laststatus);
		}
	}
	// (transient failures: no latch, no count -- retried on the next backoff window)
	LOG_I("notify", "%s (%s, %.1fkm)\n", laststatus, ser, distkm);
}

String ConnNotify::getStatus() {
	if (!sonde.config.notify.active) return String("disabled");
	if (sonde.config.notify.topic[0] == 0) return String("no topic set");
	return String(laststatus);
}

String ConnNotify::getName() { return String("Notify"); }

ConnNotify connNotify;
#endif
