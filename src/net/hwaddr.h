#ifndef TUNDRA_HWADDR_H
#define TUNDRA_HWADDR_H

#define HWADDR_LEN 6
#define HWADDR_TEXT 18

int hwaddr_current(const char *ifname, unsigned char out[HWADDR_LEN]);
int hwaddr_permanent(const char *ifname, unsigned char out[HWADDR_LEN]);
int hwaddr_random(unsigned char out[HWADDR_LEN]);
int hwaddr_is_up(const char *ifname);
void hwaddr_format(const unsigned char mac[HWADDR_LEN], char out[HWADDR_TEXT]);

#endif
