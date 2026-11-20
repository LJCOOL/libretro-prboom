/* Routing host: asserts which -iwad / -file / -deh arguments
 * retro_load_game builds for a content path.  Each case lays out a
 * card in a fresh temp dir, loads it in a forked child, and reads the
 * "retro_load_game: argv:" log line the core emits before engine
 * setup.  The child exits as soon as that line arrives, so synthetic
 * PWADs never reach the engine.
 *
 *   cc -o wad_routing_test wad_routing_test.c -ldl
 *   ./wad_routing_test ./prboom_libretro.so /dir/with/freedoom1+2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "libretro.h"

#define ARGV_TAG "retro_load_game: argv:"

static const char *core_path;
static const char *fd_dir;
static const char *child_sysdir;
static int child_pipe = -1;
static int failures;

static char base[1024], R[1100], S[1100];

/* Ring of scratch buffers so a call can build several paths inline. */
static const char *P(const char *fmt, ...)
{
   static char ring[8][2048];
   static int next;
   char *buf = ring[next++ & 7];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(ring[0]), fmt, ap);
   va_end(ap);
   return buf;
}

static void put32(FILE *f, uint32_t v)
{
   unsigned char b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24 };
   fwrite(b, 1, 4, f);
}

/* A WAD with header `ident` and one lump per name.  PLAYPAL gets a
 * full 768-byte palette; every other lump is empty. */
static void write_wad(const char *path, const char *ident,
                      const char *const *lumps, int n)
{
   FILE *f = fopen(path, "wb");
   uint32_t pos = 12, sizes[16];
   uint32_t offs[16];
   int i;
   if (!f) { perror(path); exit(2); }
   for (i = 0; i < n; i++)
   {
      sizes[i] = strcmp(lumps[i], "PLAYPAL") ? 0 : 768;
      offs[i]  = pos;
      pos     += sizes[i];
   }
   fwrite(ident, 1, 4, f);
   put32(f, n);
   put32(f, pos);
   for (i = 0; i < n; i++)
   {
      uint32_t k;
      for (k = 0; k < sizes[i]; k++)
         fputc(0, f);
   }
   for (i = 0; i < n; i++)
   {
      char name[8] = { 0 };
      strncpy(name, lumps[i], 8);
      put32(f, offs[i]);
      put32(f, sizes[i]);
      fwrite(name, 1, 8, f);
   }
   fclose(f);
}

static void write_text(const char *path, const char *text)
{
   FILE *f = fopen(path, "w");
   if (!f) { perror(path); exit(2); }
   fputs(text, f);
   fclose(f);
}

/* A real Freedoom IWAD at `dst`. */
static void freedoom(const char *dst, const char *which)
{
   if (symlink(P("%s/%s", fd_dir, which), dst)) { perror(dst); exit(2); }
}

static void dir(const char *path)
{
   if (mkdir(path, 0755)) { perror(path); exit(2); }
}

static void pwad(const char *path, const char *lump)
{
   const char *lumps[1];
   lumps[0] = lump;
   write_wad(path, "PWAD", lumps, 1);
}

/* Fresh card for one case: base/R is the ROM folder, base/S the
 * (empty unless a case fills it) system directory. */
static void setup(void)
{
   const char *tmp = getenv("TMPDIR");
   size_t L;
   snprintf(base, sizeof(base), "%s", tmp && *tmp ? tmp : "/tmp");
   L = strlen(base);
   while (L > 1 && base[L - 1] == '/')
      base[--L] = '\0';
   snprintf(base + L, sizeof(base) - L, "/wad_routing.XXXXXX");
   if (!mkdtemp(base)) { perror("mkdtemp"); exit(2); }
   snprintf(R, sizeof(R), "%s/R", base);
   snprintf(S, sizeof(S), "%s/S", base);
   dir(R);
   dir(S);
}

static void teardown(void)
{
   if (system(P("rm -rf '%s'", base))) { }
}

/* ---- child side: a minimal frontend ---- */

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   char buf[8192];
   va_list ap;
   (void)level;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   if (!strncmp(buf, ARGV_TAG, strlen(ARGV_TAG)))
   {
      const char *s = buf + strlen(ARGV_TAG);
      if (write(child_pipe, s, strlen(s))) { }
      _exit(0);
   }
}

static void video_refresh(const void *d, unsigned w, unsigned h, size_t p)
{
   (void)d; (void)w; (void)h; (void)p;
}
static void audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch(const int16_t *d, size_t f) { (void)d; return f; }
static void input_poll(void) { }
static int16_t input_state(unsigned p, unsigned d, unsigned i, unsigned id)
{
   (void)p; (void)d; (void)i; (void)id; return 0;
}

static bool environ_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback*)data)->log = log_cb;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char**)data = child_sysdir;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
         ((struct retro_variable*)data)->value = NULL;
         return false;
      default:
         break;
   }
   return false;
}

#define SYM(h, n) do { \
   *(void**)(&n) = dlsym(h, #n); \
   if (!n) _exit(3); \
} while (0)

static void child_load(const char *content)
{
   void *h;
   struct retro_game_info info;
   void (*retro_init)(void);
   bool (*retro_load_game)(const struct retro_game_info*);
   void (*retro_set_environment)(retro_environment_t);
   void (*retro_set_video_refresh)(retro_video_refresh_t);
   void (*retro_set_audio_sample)(retro_audio_sample_t);
   void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*retro_set_input_poll)(retro_input_poll_t);
   void (*retro_set_input_state)(retro_input_state_t);

   alarm(30);
   h = dlopen(core_path, RTLD_NOW);
   if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); _exit(3); }
   SYM(h, retro_init);
   SYM(h, retro_load_game);
   SYM(h, retro_set_environment);
   SYM(h, retro_set_video_refresh);
   SYM(h, retro_set_audio_sample);
   SYM(h, retro_set_audio_sample_batch);
   SYM(h, retro_set_input_poll);
   SYM(h, retro_set_input_state);

   retro_set_environment(environ_cb);
   retro_set_video_refresh(video_refresh);
   retro_set_audio_sample(audio_sample);
   retro_set_audio_sample_batch(audio_batch);
   retro_set_input_poll(input_poll);
   retro_set_input_state(input_state);
   retro_init();

   memset(&info, 0, sizeof(info));
   info.path = content;
   retro_load_game(&info);
   _exit(1);  /* load returned without logging argv */
}

/* Loads `content` with `sysdir` as the system and save directory.
 * Returns 0 and the logged arguments (text after "argv:") in argv_out,
 * or -1 if the core never logged them. */
static int run_case(const char *content, const char *sysdir,
                    char *argv_out, size_t n)
{
   int fds[2];
   size_t got = 0;
   ssize_t r;
   pid_t pid;

   if (pipe(fds)) { perror("pipe"); exit(2); }
   fflush(NULL);
   pid = fork();
   if (pid < 0) { perror("fork"); exit(2); }
   if (pid == 0)
   {
      close(fds[0]);
      child_pipe   = fds[1];
      child_sysdir = sysdir;
      child_load(content);
   }
   close(fds[1]);
   while (got + 1 < n && (r = read(fds[0], argv_out + got, n - 1 - got)) > 0)
      got += r;
   argv_out[got] = '\0';
   close(fds[0]);
   waitpid(pid, NULL, 0);
   while (got > 0 && (argv_out[got - 1] == '\n' || argv_out[got - 1] == '\r'))
      argv_out[--got] = '\0';
   return got ? 0 : -1;
}

/* ---- assertions ---- */

/* `needle` appears in `argv` as a run of whole arguments. */
static int has(const char *argv, const char *needle)
{
   char hay[8192], pin[4096];
   snprintf(hay, sizeof(hay), " %s ", argv);
   snprintf(pin, sizeof(pin), " %s ", needle);
   return strstr(hay, pin) != NULL;
}

static void check(const char *name, int ok, const char *argv)
{
   if (ok)
      printf("ok %s\n", name);
   else
   {
      printf("FAIL %s: got %s\n", name, argv);
      failures++;
   }
}

/* ---- cases ---- */

static void single_iwad(void)
{
   char a[8192];
   setup();
   freedoom(P("%s/freedoom2.wad", R), "freedoom2.wad");
   run_case(P("%s/freedoom2.wad", R), S, a, sizeof(a));
   check("single_iwad", has(a, "-iwad freedoom2.wad"), a);
   teardown();
}

static void single_pwad_doom2(void)
{
   char a[8192];
   setup();
   freedoom(P("%s/freedoom1.wad", R), "freedoom1.wad");
   freedoom(P("%s/freedoom2.wad", R), "freedoom2.wad");
   pwad(P("%s/mod.wad", R), "MAP01");
   run_case(P("%s/mod.wad", R), S, a, sizeof(a));
   check("single_pwad_doom2",
         has(a, P("-iwad %s/freedoom2.wad -file %s/mod.wad", R, R)), a);
   teardown();
}

static void single_pwad_doom1(void)
{
   char a[8192];
   setup();
   freedoom(P("%s/freedoom1.wad", R), "freedoom1.wad");
   freedoom(P("%s/freedoom2.wad", R), "freedoom2.wad");
   pwad(P("%s/ep.wad", R), "E1M1");
   run_case(P("%s/ep.wad", R), S, a, sizeof(a));
   check("single_pwad_doom1", has(a, P("-iwad %s/freedoom1.wad", R)), a);
   teardown();
}

static void single_pwad_sidecar_deh(void)
{
   char a[8192];
   setup();
   freedoom(P("%s/freedoom2.wad", R), "freedoom2.wad");
   pwad(P("%s/mod.wad", R), "MAP01");
   write_text(P("%s/mod.deh", R), "Patch File for DeHackEd v3.0\n");
   run_case(P("%s/mod.wad", R), S, a, sizeof(a));
   check("single_pwad_sidecar_deh", has(a, P("-deh %s/mod.deh", R)), a);
   teardown();
}

/* `-iwad` followed by `path`, compared without case: on a
 * case-insensitive filesystem the core may name the file in either. */
static int has_iwad_ci(const char *argv, const char *path)
{
   char hay[8192], pin[4096];
   size_t i, hn, pn;
   snprintf(hay, sizeof(hay), " %s ", argv);
   snprintf(pin, sizeof(pin), " -iwad %s ", path);
   hn = strlen(hay);
   pn = strlen(pin);
   for (i = 0; i + pn <= hn; i++)
      if (!strncasecmp(hay + i, pin, pn))
         return 1;
   return 0;
}
static void parent_search_doom2(void)
{
   char a[8192];
   setup();
   freedoom(P("%s/freedoom2.wad", R), "freedoom2.wad");
   dir(P("%s/Mod", R));
   pwad(P("%s/Mod/mod.wad", R), "MAP01");
   run_case(P("%s/Mod/mod.wad", R), S, a, sizeof(a));
   check("parent_search_doom2",
         has(a, P("-iwad %s/freedoom2.wad -file %s/Mod/mod.wad", R, R)), a);
   teardown();
}

static void parent_search_same_dir_first(void)
{
   char a[8192];
   setup();
   freedoom(P("%s/freedoom2.wad", R), "freedoom2.wad");
   dir(P("%s/Mod", R));
   freedoom(P("%s/Mod/freedoom2.wad", R), "freedoom2.wad");
   pwad(P("%s/Mod/mod.wad", R), "MAP01");
   run_case(P("%s/Mod/mod.wad", R), S, a, sizeof(a));
   check("parent_search_same_dir_first",
         has(a, P("-iwad %s/Mod/freedoom2.wad", R)), a);
   teardown();
}

static void parent_search_before_sysdir(void)
{
   char a[8192];
   setup();
   freedoom(P("%s/freedoom2.wad", S), "freedoom2.wad");
   freedoom(P("%s/freedoom2.wad", R), "freedoom2.wad");
   dir(P("%s/Mod", R));
   pwad(P("%s/Mod/mod.wad", R), "MAP01");
   run_case(P("%s/Mod/mod.wad", R), S, a, sizeof(a));
   check("parent_search_before_sysdir",
         has(a, P("-iwad %s/freedoom2.wad", R)), a);
   teardown();
}

static void parent_search_uppercase(void)
{
   char a[8192];
   setup();
   freedoom(P("%s/FREEDOOM2.WAD", R), "freedoom2.wad");
   dir(P("%s/Mod", R));
   pwad(P("%s/Mod/MOD.WAD", R), "MAP01");
   run_case(P("%s/Mod/MOD.WAD", R), S, a, sizeof(a));
   check("parent_search_uppercase",
         has_iwad_ci(a, P("%s/FREEDOOM2.WAD", R)), a);
   teardown();
}

static void parent_search_one_level_only(void)
{
   char a[8192];
   setup();
   freedoom(P("%s/freedoom2.wad", R), "freedoom2.wad");
   dir(P("%s/Packs", R));
   dir(P("%s/Packs/Mod", R));
   pwad(P("%s/Packs/Mod/mod.wad", R), "MAP01");
   run_case(P("%s/Packs/Mod/mod.wad", R), S, a, sizeof(a));
   check("parent_search_one_level_only", !strstr(a, "-iwad"), a);
   teardown();
}

int main(int argc, char **argv)
{
   if (argc < 3)
   {
      fprintf(stderr, "usage: %s core.so freedoom-dir\n", argv[0]);
      return 2;
   }
   core_path = argv[1];
   fd_dir    = argv[2];

   single_iwad();
   single_pwad_doom2();
   single_pwad_doom1();
   single_pwad_sidecar_deh();
   parent_search_doom2();
   parent_search_same_dir_first();
   parent_search_before_sysdir();
   parent_search_uppercase();
   parent_search_one_level_only();

   if (failures)
   {
      printf("%d case(s) failed\n", failures);
      return 1;
   }
   printf("PASS\n");
   return 0;
}
