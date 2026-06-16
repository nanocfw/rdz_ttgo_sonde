#include "user.h"

#define TAG "user"
#include "logger.h"
#include <LittleFS.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/md.h>

// User management function (simple web authentication)
// As secure as it can be with just http:
// Login form creates a random preauth ticket (nonce)
// Client-side form calculates SHA256(username:preauth:password) as authenticator
// Upon receiving correct authenticator, TTGO generates session ID and replaces preauth ticket with session ID in internal cookie store
// (i.e. preauth ticket can be used only once)
// TODO (optional) restrict session ID to specific client ID
// Session expiry is a sliding idle timeout: each valid request refreshes the expiry, so a
// session only ends after COOKIE_EXPIRY_DURATION of inactivity (see getCookieAuthLevel).

// USERLEN, RNDLEN and COOKIE_SIZE are defined in user.h

struct SessionCookie {
  char value[USERLEN+RNDLEN+2];
  char userclass;    // -1: preauth; 0=none/locked, 1=r/o user, 2=admin
  unsigned long expiry;
};

const int MAX_SESSIONS = 4;
SessionCookie authCookies[MAX_SESSIONS];
int cookieCount = 0;

const unsigned long COOKIE_EXPIRY_DURATION = 30 * 60 * 1000; // 30 minutes in milliseconds
const unsigned long PREAUTH_EXPIRY_DURATION = 60 * 1000;  // 1 minute in milliseconds

static const char *getUser(const char *user, char *line, int maxlen, int *outLevel);

void cleanupExpiredCookies() {
  unsigned long now = millis();
  int i = 0;

  // Loop through the cookies and remove expired ones
  while (i < cookieCount) {
    // Signed difference handles millis() wraparound (~49 days uptime) correctly.
    if ((long)(now - authCookies[i].expiry) > 0) {
      // Shift the remaining cookies left
      for (int j = i; j < cookieCount - 1; j++) {
        authCookies[j] = authCookies[j + 1];
      }
      cookieCount--; // Reduce the count of cookies
    } else {
      i++;
    }
  }
}

void storeCookie(const char *cookie, char userclass) {
  cleanupExpiredCookies();
  if (cookieCount >= MAX_SESSIONS) {
    LOG_D(TAG, "No space for new cookie. Cleaning up oldest cookies.");
    for(int i=1; i<MAX_SESSIONS; i++) { 
      authCookies[i-1] = authCookies[i];
    }
    cookieCount--;
  }
  // Copy cookie string into the fixed-size array
  strlcpy(authCookies[cookieCount].value, cookie, sizeof(authCookies[cookieCount].value));
  authCookies[cookieCount].userclass = userclass;
  authCookies[cookieCount].expiry = millis() + (userclass==-1?PREAUTH_EXPIRY_DURATION:COOKIE_EXPIRY_DURATION);
  cookieCount++;
  LOG_D(TAG, "Cookie stored: %s, Expiry in: %lu ms\n", cookie, COOKIE_EXPIRY_DURATION);
}

int upgradeCookie(const char *preauth, const char *cookie, char userclass) {
  for (int i = 0; i < cookieCount; i++) {
    if (strcmp(preauth, authCookies[i].value)==0) {
      strlcpy(authCookies[i].value, cookie, sizeof(authCookies[i].value));
      authCookies[i].userclass = userclass;
      // Refresh expiry: the entry still carries the short preauth lifetime, so without this
      // the upgraded session would expire ~1 minute after the login page was loaded.
      authCookies[i].expiry = millis() + COOKIE_EXPIRY_DURATION;
      return 0;
    }
  }
  return -1;
}

char allcn[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

// Function to generate a random session cookie
void generateRandomCookie(const char *user, char *cookie) {
  int j = 0;
  for (int i = 0; i < USERLEN && user[i]; i++, j++) {
    cookie[j] = user[i];
  }
  cookie[j++] = ':';
  for (int i = 0; i < RNDLEN; ++i) {
    cookie[j++] = allcn[random(sizeof(allcn)-1)];  // sizeof includes \0
  }
  cookie[j++] = '\0';
}

// -1: invalid user; 0=PERM_NONE, 1=PERM_RO, 2=PERM_ADMIN
int getCookieAuthLevel(const char *cookie) {
  unsigned long now = millis();
  for (int i = 0; i < cookieCount; i++) { 
    if (strcmp(authCookies[i].value, cookie) == 0) {
      // Signed difference handles millis() wraparound (~49 days uptime) correctly.
      if ((long)(now - authCookies[i].expiry) < 0) {
        // Sliding idle timeout: refresh expiry on every valid use so the session only
        // expires after COOKIE_EXPIRY_DURATION of inactivity (preauth tickets keep their
        // short, single-use lifetime and are never extended here).
        if (authCookies[i].userclass != -1) {
          authCookies[i].expiry = now + COOKIE_EXPIRY_DURATION;
        }
        return authCookies[i].userclass; // Valid and not expired
      } else {
        // Cookie expired, remove it
        for (int j = i; j < cookieCount - 1; j++) {
          authCookies[j] = authCookies[j + 1];
        }
        cookieCount--; // Decrement cookie count
        break; // Exit the loop after removing expired cookie
      }
    }
  }
  return -1;
}


// Remove a session cookie from the store (used for logout).
// Returns 0 if a matching cookie was removed, -1 if not found.
int removeCookie(const char *cookie) {
  for (int i = 0; i < cookieCount; i++) {
    if (strcmp(authCookies[i].value, cookie) == 0) {
      for (int j = i; j < cookieCount - 1; j++) {
        authCookies[j] = authCookies[j + 1];
      }
      cookieCount--;
      return 0;
    }
  }
  return -1;
}

// -1: user does not exist; 0=PERM_NONE, 1=PERM_RO, 2=PERM_ADMIN
int getUserPermissions(const char *user, const char *preauth, const char *auth) {
    // simple digest authentication:
    // (we want to avoid sending plain text passwords via http)
    // digest is SHA256(user:preauth:password)
    char buf[256];
    char line[128];
    int level = 0;
    const char *pass = getUser(user, line, 128, &level);
    if(!pass) {  // user not found
      return -1;
    }
    // auth is user-provided: it must be exactly 64 hex chars (SHA256 = 32 bytes).
    // Reject anything else to avoid reading past the end of the string below.
    if(strlen(auth) != 64) {
      return -1;
    }
    strlcpy(buf, user, 256);
    strlcat(buf, ":", 256);
    strlcat(buf, preauth, 256);  // TODO: Check if it exists? (well upgrade will fail if not...)
    strlcat(buf, ":", 256);   // TODO bound checks.... this is user provided data!!!!!!
    strlcat(buf, pass, 256); 
    unsigned char sharesult[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const unsigned char *) buf, strlen(buf));
    mbedtls_md_finish(&ctx, sharesult);
    bool match = true;
    for(int i= 0; i< sizeof(sharesult); i++){
      char str[3];
      sprintf(str, "%02x", (int)sharesult[i]);
      if( (auth[0]!=str[0]) || (auth[1]!=str[1])) match = false;
      auth += 2;
    }
    int authres = match ? level : -1;
    LOG_I(TAG, "login: match: %d => auth level %d\n", match, authres);
    return authres;
}

// Find user intry in password file
extern int readLine(Stream &stream, char *buffer, int maxlen);  // impl in RX_FSK.ino


// Trim leading/trailing ASCII whitespace in place; returns a pointer to the first non-space char.
static char *trimws(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  int n = strlen(s);
  while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
  return s;
}

// Find a user entry in the password file.
// File format: one "username,level,password" record per line; lines starting with '#' are comments.
// Leading/trailing whitespace around each field is ignored, the level may have multiple digits,
// and trailing CR is already stripped by readLine().
// On success returns a pointer to the (null-terminated) password and, if outLevel != NULL, stores
// the parsed access level there. Returns NULL if the user is not found.
static const char *getUser(const char *user, char *line, int maxlen, int *outLevel) {
  File file = LittleFS.open("/user.txt", "r");
  if(!file) {
    LOG_E(TAG, "Error opening '/user.txt'\n");
    if(outLevel) *outLevel = 2;  // all permissions by default if no user file exists
    line[0] = 0;
    return line;                 // empty password
  }
  while (file.available()) {
    int res = readLine(file, line, maxlen);
    if(res <= 0) continue;
    if(line[0] == '#') continue;
    // Split into three fields at the first two commas
    char *sep1 = strchr(line, ',');
    if(!sep1) continue;          // malformed: no level/password
    *sep1 = 0;
    char *sep2 = strchr(sep1 + 1, ',');
    if(!sep2) continue;          // malformed: no password
    *sep2 = 0;
    char *uname = trimws(line);
    char *lvl   = trimws(sep1 + 1);
    char *pass  = trimws(sep2 + 1);
    if(strcmp(user, uname) == 0) {
      LOG_D(TAG, "Found pw entry for user '%s'\n", user);
      if(outLevel) {
        char *end;
        long v = strtol(lvl, &end, 10);
        if(end == lvl || v < 0) v = 0;  // non-numeric/invalid level => no access
        *outLevel = (int)v;
      }
      return pass;
    }
  }
  return NULL;
}

int getDefaultAuthLevel() {
  char line[128];
  int level = 2;
  const char *ptr = getUser("", line, 128, &level);
  // No explicit default line: lock anonymous access out (level 0) once any account exists,
  // otherwise keep it open (level 2) so a fresh/empty device can still be set up.
  if(!ptr) return hasNamedUsers() ? 0 : 2;
  return level;
}

// ---- User management (add/remove/list named users in /user.txt) ----

#define MAX_USER_LINE 160

// Return the username field (up to the first comma, trimmed) of a record line.
// Writes into dst (size dstlen) and returns it. Empty string for comment/blank lines.
static const char *lineUsername(const char *line, char *dst, int dstlen) {
  strlcpy(dst, line, dstlen);
  char *comma = strchr(dst, ',');
  if(comma) *comma = 0;
  return trimws(dst);
}

// Rewrite /user.txt via a temp file, applying one change to the record for `user`:
//  - newline != NULL : replace the matching line, or append it if `user` was not present
//  - newline == NULL : delete the matching line
// Comment lines, blank lines and all other records are preserved verbatim.
// Returns 1 if an existing record matched, 0 if appended/not-found, -1 on filesystem error.
static int rewriteUserFile(const char *user, const char *newline) {
  File in = LittleFS.open("/user.txt", "r");
  File out = LittleFS.open("/user.tmp", "w");
  if(!out) { if(in) in.close(); return -1; }
  bool matched = false;
  char line[MAX_USER_LINE];
  char namebuf[MAX_USER_LINE];
  if(in) {
    while(in.available()) {
      int n = readLine(in, line, MAX_USER_LINE);
      if(n <= 0) continue;                       // drop blank lines
      if(line[0] == '#') { out.printf("%s\n", line); continue; }  // keep comments
      const char *uname = lineUsername(line, namebuf, MAX_USER_LINE);
      if(strcmp(uname, user) == 0) {
        matched = true;
        if(newline) out.printf("%s\n", newline); // replace; for delete, write nothing
      } else {
        out.printf("%s\n", line);                // keep other records verbatim
      }
    }
    in.close();
  }
  if(!matched && newline) out.printf("%s\n", newline);  // append new user
  out.close();
  LittleFS.remove("/user.txt");
  if(!LittleFS.rename("/user.tmp", "/user.txt")) return -1;
  return matched ? 1 : 0;
}

// Count named users with admin (level >= 2) access.
static int countAdmins() {
  File in = LittleFS.open("/user.txt", "r");
  if(!in) return 0;
  char line[MAX_USER_LINE];
  int count = 0;
  while(in.available()) {
    int n = readLine(in, line, MAX_USER_LINE);
    if(n <= 0 || line[0] == '#') continue;
    char *c1 = strchr(line, ',');
    if(!c1) continue;
    *c1 = 0;
    char *c2 = strchr(c1 + 1, ',');
    if(!c2) continue;
    *c2 = 0;
    if(trimws(line)[0] == 0) continue;                 // skip the default entry
    if((int)strtol(trimws(c1 + 1), NULL, 10) >= 2) count++;
  }
  in.close();
  return count;
}

// Current access level of a named user, or -1 if not present.
static int currentUserLevel(const char *user) {
  char line[MAX_USER_LINE];
  int level = -1;
  if(!hasNamedUsers()) return -1;                       // no user file/records => treat as absent
  const char *p = getUser(user, line, MAX_USER_LINE, &level);
  return p ? level : -1;
}

// Add or update a named user.
// Returns 0 on success, -1 on invalid input/error, -2 if it would demote the last administrator.
int setUser(const char *user, int level, const char *password) {
  if(!user || user[0] == 0 || !password) return -1;   // empty username = default entry, not managed here
  if(level < 1 || level > 2) return -1;
  // The first account must be an administrator, otherwise nobody could ever manage users again.
  bool firstUser = !hasNamedUsers();
  if(firstUser) level = 2;
  // Don't allow demoting the last administrator (would lock out user management).
  if(level < 2 && currentUserLevel(user) >= 2 && countAdmins() <= 1) return -2;
  // The flat file is comma-separated and line-based: reject anything that would corrupt it.
  if(strpbrk(user, ",\r\n") || strpbrk(password, ",\r\n")) return -1;
  if(strchr(user, '"')) return -1;                    // keep JSON listing simple/safe
  if(strlen(user) + strlen(password) + 8 >= MAX_USER_LINE) return -1;
  char newline[MAX_USER_LINE];
  snprintf(newline, MAX_USER_LINE, "%s,%d,%s", user, level, password);
  if(rewriteUserFile(user, newline) < 0) return -1;
  // As soon as the first real account exists, close the wide-open default: set the
  // empty-username default line to level 0 so unauthenticated clients lose config access.
  // (Before this, a fresh device ships ",2," so initial setup is possible without login.)
  if(firstUser) rewriteUserFile("", ",0,");
  return 0;
}

// Delete a named user.
// Returns 0 if removed, -1 if not found or on error, -2 if it would remove the last administrator.
int deleteUser(const char *user) {
  if(!user || user[0] == 0) return -1;
  // Don't allow removing the last administrator (would lock out user management).
  if(currentUserLevel(user) >= 2 && countAdmins() <= 1) return -2;
  return rewriteUserFile(user, NULL) == 1 ? 0 : -1;
}

// Build a JSON array of named users (passwords are never included): [{"user":"x","level":N},...]
// Writes into out (size outlen) and returns the length written.
int getUserListJson(char *out, int outlen) {
  int len = snprintf(out, outlen, "[");
  File in = LittleFS.open("/user.txt", "r");
  char line[MAX_USER_LINE];
  bool first = true;
  if(in) {
    while(in.available()) {
      int n = readLine(in, line, MAX_USER_LINE);
      if(n <= 0 || line[0] == '#') continue;
      char *c1 = strchr(line, ',');
      if(!c1) continue;
      *c1 = 0;
      char *c2 = strchr(c1 + 1, ',');
      if(!c2) continue;
      *c2 = 0;
      char *uname = trimws(line);
      if(uname[0] == 0) continue;               // skip the default (unauthenticated) entry
      int lvl = (int)strtol(trimws(c1 + 1), NULL, 10);
      if(len > outlen - 64) break;              // leave room; stop if buffer is nearly full
      len += snprintf(out + len, outlen - len, "%s{\"user\":\"%s\",\"level\":%d}",
                      first ? "" : ",", uname, lvl);
      first = false;
    }
    in.close();
  }
  len += snprintf(out + len, outlen - len, "]");
  return len;
}

// True if /user.txt contains at least one named (non-empty username) user record.
// Used to decide whether the device is still in its "clean" bootstrap state.
bool hasNamedUsers() {
  File in = LittleFS.open("/user.txt", "r");
  if(!in) return false;
  char line[MAX_USER_LINE];
  char namebuf[MAX_USER_LINE];
  bool found = false;
  while(in.available()) {
    int n = readLine(in, line, MAX_USER_LINE);
    if(n <= 0 || line[0] == '#') continue;
    const char *uname = lineUsername(line, namebuf, MAX_USER_LINE);
    if(uname[0] != 0) { found = true; break; }
  }
  in.close();
  return found;
}

