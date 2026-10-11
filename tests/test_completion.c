#include "rapelz.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    rapelz_alias_init();
    if (rapelz_alias_set("rapelzalias", "echo alias") != 0) return 2;

    char b[256];
    size_t len, cur;

    strcpy(b, "rapelzalia");
    len = cur = strlen(b);
    (void)rapelz_complete(b, &len, &cur, sizeof(b));
    if (strncmp(b, "rapelzalias", 11) != 0) return 3;

    strcpy(b, "~");
    len = cur = 1;
    if (rapelz_complete(b, &len, &cur, sizeof(b)) == 0) return 4;
    if (strncmp(b, "~/", 2) != 0) return 5;

    rapelz_alias_shutdown();
    puts("OK: completion dasar lulus");
    return 0;
}
