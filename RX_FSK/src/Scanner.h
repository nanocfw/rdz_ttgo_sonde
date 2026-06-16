

#ifndef _SCANNER_H
#define _SCANNER_H

#include <stdlib.h>
#include <stdint.h>
#include <Arduino.h>
#ifndef inttypes_h
        #include <inttypes.h>
#endif
class Scanner
{
private:
	void fillTiles(uint8_t *row, int value);

public:
	void plotResult();
	void scan(void);
	// Web scan-plot support
	void scanForWeb();		// data-only sweep (no display), bumps the sequence counter
	uint32_t webSeq();		// increments on each completed web sweep
	uint32_t webMillis();		// millis() at the last web sweep (0 if none yet)
	int dispW();			// number of display bins in scandisp[]
	double stepMHz();		// MHz per display bin
	const int *dispData();		// pointer to scandisp[] (dispW() entries)
	double peakMHz();		// peak frequency in MHz
};

extern Scanner scanner;
#endif
