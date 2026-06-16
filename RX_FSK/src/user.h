
// Session/preauth cookie layout: up to USERLEN username chars + ':' + RNDLEN random chars + '\0'.
// COOKIE_SIZE must stay in sync with the SessionCookie.value buffer in user.cpp, so derive both
// from the same macros instead of hardcoding the length.
#define USERLEN 8
#define RNDLEN 16
#define COOKIE_SIZE (USERLEN+RNDLEN+2)

enum { PERM_NONE, PERM_RO, PERM_ADMIN } permissions;

void cleanupExpiredCookies();
void storeCookie(const char *cookie, char userclass);
int upgradeCookie(const char *preauth, const char *cookie, char userclass);
void generateRandomCookie(const char *user, char *cookie);
int getCookieAuthLevel(const char *cookie);
int removeCookie(const char *cookie);
int getUserPermissions(const char *user, const char *preauth, const char *auth);
int getDefaultAuthLevel();
