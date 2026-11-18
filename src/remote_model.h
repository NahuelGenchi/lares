#pragma once
#include <gio/gio.h>

/* What the Remote section manages: the screen size, Sunshine and Tailscale,
 * and the Home and Remote modes that set all three at once. */

typedef enum { ITEM_SCREEN, ITEM_SUNSHINE, ITEM_TAILSCALE, N_ITEMS } Item;
typedef enum { MODE_HOME, MODE_REMOTE, N_MODES } Mode;

typedef struct {
    char *id;
    int w, h;
    double rate;
    double *scales;
    gsize n_scales;
    gboolean current, preferred;
} ScreenMode;

/* The system as it is right now. -1 means not read yet. */
typedef struct {
    gboolean screen_known;
    char *connector;
    int w, h;
    double rate;
    GPtrArray *modes;      /* ScreenMode*, of the main monitor */
    int sunshine, tailscale;
    gboolean streaming;    /* a Moonlight client is connected to Sunshine */
    /* For the setup guide */
    gboolean tailscale_installed, tailscale_signed_in;
    char *tailscale_host, *tailscale_ip;   /* this PC's name and address on the tailnet */
    gboolean sunshine_installed;
    gboolean paired;       /* a Moonlight device is paired with Sunshine */
} Live;

/* ~/.config/lares/lares.ini. A screen value is "WxH@Hz" ("leave" only
 * until the first reading fills in the monitor's own size); a service is
 * "on" or "off". */
typedef struct {
    Mode mode;
    gboolean mode_known;
    gboolean ready;        /* the setup guide is done */
    char *values[N_MODES][N_ITEMS];
    char *sunshine_unit;   /* NULL: found among the usual names */
    char *path;
} Config;

typedef struct Remote Remote;
typedef void (*StepDone)(gboolean ok, gboolean needs_permission, const char *message, gpointer user);
typedef void (*Notify)(gpointer user);

Remote *remote_new(void);
Config *remote_config(Remote *r);
Live *remote_live(Remote *r);
void remote_save(Remote *r);

/* Tailscale signed in, Sunshine installed and a Moonlight device paired. */
gboolean remote_setup_done(Remote *r);

/* Reads the live state once; `done` runs after it. */
void remote_read_live(Remote *r, Notify done, gpointer user);
/* Watchers run after every read and every screen change. */
void remote_watch(Remote *r, Notify fn, gpointer user);
/* While anyone wants it, the live state is read every few seconds. */
void remote_want(Remote *r, gboolean on);
void remote_refresh(Remote *r);

void remote_apply(Remote *r, Item item, const char *value, StepDone done, gpointer user);
void remote_allow_tailscale(Remote *r, StepDone done, gpointer user);

gboolean remote_matches(Remote *r, Item item, const char *value);
char *remote_describe(Item item, const char *value);
/* The value a mode uses for what was saved: a service is on or off, so a
 * missing value or an old "leave" becomes the mode's own. */
char *remote_saved_value(Mode mode, Item item, const char *saved);
/* The main monitor's own size as a screen value: its preferred mode, else
 * its current one; NULL before the screen is read. */
char *remote_own_size(const Live *live);
char *remote_describe_live(Remote *r, Item item);

const Item *apply_order(Mode mode, int *n);
const char *item_name(Item item);
const char *mode_name(Mode mode);
gboolean parse_screen(const char *spec, int *w, int *h, double *rate);
