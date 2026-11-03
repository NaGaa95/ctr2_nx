#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

static int checks;
#define CHECK(condition) do { checks++; if (!(condition)) { \
  fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

static void put_config(const char *text) {
  FILE *file = fopen("case-config.txt", "wb");
  if (!file || fputs(text, file) < 0 || fclose(file)) exit(2);
}

int main(void) {
  CHECK(read_config("does-not-exist.txt") == -1);
  CHECK(config.portrait == DEFAULT_PORTRAIT);
  CHECK(config.screen_width == -1 && config.screen_height == -1);
  const struct { const char *text; int expected; } cases[] = {
    {"portrait 0\n", 0}, {"portrait 1\n", 1}, {"portrait 2\n", 2},
    {"\tportrait\t0\r\n", 0}, {"portrait 2   \n", 2},
    {"# comment\n\nportrait 0\n", 0}, {"portrait +0\n", 0},
    {"portrait banana\n", 1}, {"portrait 0junk\n", 1},
    {"portrait 1junk\n", 1}, {"portrait 1.0\n", 1},
    {"portrait 0x0\n", 1}, {"portrait -1\n", 1}, {"portrait 3\n", 1},
    {"portrait 2147483648\n", 1}, {"portrait -2147483649\n", 1},
    {"portrait 99999999999999999999999999999999\n", 1},
    {"portrait -99999999999999999999999999999999\n", 1},
    {"portrait\n", 1}, {"portrait \n", 1}, {"portrait", 1},
    {"portrait\t  \r\n", 1}, {"\n  \t\r\n", 1},
    {"portrait 0\nportrait invalid\n", 1},
    {"portrait invalid\nportrait 2\n", 2},
    {"screen_width -1\nscreen_height -1\nlanguage 2\nportrait 0\n", 0}
  };
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    put_config(cases[i].text);
    CHECK(read_config("case-config.txt") == 0);
    CHECK(config.portrait == cases[i].expected);
  }
  put_config("portrait 0\nwidescreen 1\n");
  CHECK(read_config("case-config.txt") == 1);
  CHECK(config.portrait == 0);
  CHECK(write_config("rewritten-config.txt") == 0);
  CHECK(read_config("rewritten-config.txt") == 0);
  CHECK(config.portrait == 0);
  put_config("");
  CHECK(read_config("case-config.txt") == 0);
  CHECK(config.portrait == DEFAULT_PORTRAIT);
  CHECK(write_config("generated-config.txt") == 0);
  CHECK(read_config("generated-config.txt") == 0);
  CHECK(config.portrait == DEFAULT_PORTRAIT);
  printf("PASS: %d configuration checks\n", checks);
  return 0;
}
