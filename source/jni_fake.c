/* jni_fake.c -- Android/JNI compatibility environment for ZFramework.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "asset_pack.h"
#include "jni_unimpl.h"
#include "libc_shim.h"   /* managed_path: device-less paths for managed code */
#include "unity_jni.h"
#include "zf_audio.h"
#include "ctr_video.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;
typedef union {
  uint8_t z; int8_t b; uint16_t c; int16_t s; int32_t i; int64_t j;
  float f; double d; void *l;
} FakeJValue;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  pooled, never freed
  TAG_BILLING_PRODUCT = 0x42505244, // 'BPRD' pooled offline ProductDetails
  TAG_BILLING_OFFER   = 0x424f4646, // 'BOFF' pooled one-time offer/pricing phase
  TAG_BILLING_LIST    = 0x424c5354, // 'BLST' pooled one-element java.util.List
};

typedef struct { uint32_t tag; char label[64]; } FakeObject;
/* Reference-counted JNI strings and arrays. */
typedef struct { uint32_t tag; uint32_t refs; char *utf; } FakeString;
typedef struct { uint32_t tag; uint32_t refs; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; uint32_t refs; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; uint32_t refs; } FakeRefHeader;
typedef struct { uint32_t tag; char cls[96]; char name[64]; char sig[160]; } FakeID;
typedef struct { uint32_t tag; char name[96]; } FakeClass;

#define OFFLINE_BILLING_MAX_PRODUCTS 128
#define OFFLINE_BILLING_ID_LENGTH 192

typedef struct {
  uint32_t tag;
  char product_id[OFFLINE_BILLING_ID_LENGTH];
} FakeBillingItem;

typedef struct {
  uint32_t tag;
  FakeBillingItem *item;
} FakeBillingList;

typedef void (*BillingVoidFn)(void *, void *);
typedef void (*BillingBoolFn)(void *, void *, uint8_t);
typedef void (*BillingStringFn)(void *, void *, void *);
typedef void (*BillingObjectFn)(void *, void *, void *);
typedef void (*BillingPurchaseFn)(void *, void *, void *, void *, void *);

static FakeBillingItem g_billing_products[OFFLINE_BILLING_MAX_PRODUCTS];
static FakeBillingItem g_billing_offers[OFFLINE_BILLING_MAX_PRODUCTS];
static FakeBillingList g_billing_lists[OFFLINE_BILLING_MAX_PRODUCTS];
static unsigned g_billing_item_count;

static struct {
  Mutex lock;
  void *object;
  BillingBoolFn on_setup_finished;
  BillingStringFn on_store_locale_detected;
  BillingObjectFn on_product_details_received;
  BillingVoidFn on_request_products_succeeded;
  BillingPurchaseFn on_purchase_succeeded;
  BillingVoidFn on_restore_purchases_succeeded;
  BillingStringFn on_consume_succeeded;
  BillingStringFn on_acknowledge_succeeded;
  int pending_setup;
  int pending_locale;
  int pending_products;
  int pending_purchase;
  int pending_restore;
  int pending_consume;
  int pending_acknowledge;
  unsigned purchase_serial;
  unsigned catalogue_count;
  char catalogue[OFFLINE_BILLING_MAX_PRODUCTS][OFFLINE_BILLING_ID_LENGTH];
  char purchase_id[OFFLINE_BILLING_ID_LENGTH];
  char consume_id[OFFLINE_BILLING_ID_LENGTH];
  char acknowledge_id[OFFLINE_BILLING_ID_LENGTH];
} g_offline_billing;

volatile int jni_quit_requested = 0;

typedef void (*UnityKeyboardVisibleFn)(void *, void *, uint8_t);
typedef void (*UnityInputStringFn)(void *, void *, void *);
typedef void (*UnitySoftInputFn)(void *, void *);

static UnityKeyboardVisibleFn g_unity_keyboard_visible;
static UnityInputStringFn g_unity_input_string;
static UnitySoftInputFn g_unity_soft_input_closed;
static UnitySoftInputFn g_unity_soft_input_canceled;

static struct {
  Mutex lock;
  int pending;
  int keyboard_type;
  int secure;
  int character_limit;
  char initial[0x1001];
  char guide[0x101];
} g_soft_keyboard;

/* ZF3 UI operations are deferred to the renderer thread. */
typedef void (*Zf3UiFetchFn)(void *,void *,int64_t);
static struct {
  void *object;
  int64_t native_handle;
  Zf3UiFetchFn fetch;
  unsigned pending;
} g_zf3_threads;

// ---------------------------------------------------------------------------
// Thread-local JNI reference frames.
// ---------------------------------------------------------------------------

#define MAX_LOCALS 1048576
#define MAX_FRAMES 64

typedef struct {
  void **refs;
  size_t top;
  size_t cap;
  size_t frames[MAX_FRAMES];
  int frame_top;
} LocalState;

static pthread_key_t g_local_state_key;
static int g_local_state_key_ready;
static Mutex pool_lock;

static void retain_ref(void *ref);
static void free_ref(void *ref);

static void local_state_destroy(void *opaque) {
  LocalState *state = opaque;
  if (!state)
    return;
  for (size_t i = 0; i < state->top; i++)
    free_ref(state->refs[i]);
  free(state->refs);
  free(state);
}

static LocalState *local_state_current(void) {
  return g_local_state_key_ready ? pthread_getspecific(g_local_state_key) : NULL;
}

static LocalState *local_state_get(void) {
  LocalState *state = local_state_current();
  if (state || !g_local_state_key_ready)
    return state;
  state = calloc(1, sizeof(*state));
  if (!state)
    return NULL;
  if (pthread_setspecific(g_local_state_key, state) != 0) {
    free(state);
    return NULL;
  }
  return state;
}

static int local_reserve(LocalState *state, size_t needed) {
  if (!state || needed > MAX_LOCALS)
    return 0;
  if (needed <= state->cap)
    return 1;
  size_t cap = state->cap ? state->cap : 256;
  while (cap < needed) {
    if (cap >= MAX_LOCALS / 2) {
      cap = MAX_LOCALS;
      break;
    }
    cap *= 2;
  }
  void **refs = realloc(state->refs, cap * sizeof(*refs));
  if (!refs)
    return 0;
  state->refs = refs;
  state->cap = cap;
  return 1;
}

static void *reg_local(void *ref) {
  if (ref) {
    LocalState *state = local_state_get();
    if (state && local_reserve(state, state->top + 1)) {
      state->refs[state->top++] = ref;
    }
  }
  return ref;
}

// Intern frequently reused constant strings.
#define MAX_ISTR 512
static FakeString istr_pool[MAX_ISTR];
static int istr_count = 0;

static int ref_is_counted(uint32_t tag) {
  return tag == TAG_STRING || tag == TAG_PRIARR || tag == TAG_OBJARR ||
         tag == TAG_OBJECT;
}

static void retain_ref(void *ref) {
  if (!ref)
    return;
  if ((char *)ref >= (char *)istr_pool && (char *)ref < (char *)&istr_pool[MAX_ISTR])
    return;
  FakeRefHeader *header = ref;
  if (!ref_is_counted(header->tag))
    return;  // pooled class/ID or externally-owned framework handle
  uint32_t refs = __atomic_load_n(&header->refs, __ATOMIC_RELAXED);
  while (refs && refs != UINT32_MAX) {
      if (__atomic_compare_exchange_n(&header->refs, &refs, refs + 1, 0,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
      return;
    }
  }
}

static void free_ref(void *ref) {
  if (!ref)
    return;
  if ((char *)ref >= (char *)istr_pool && (char *)ref < (char *)&istr_pool[MAX_ISTR])
    return;  // interned string -- pooled, never freed
  FakeRefHeader *header = ref;
  if (ref_is_counted(header->tag)) {
    const uint32_t previous = __atomic_fetch_sub(&header->refs, 1, __ATOMIC_ACQ_REL);
    if (previous > 1)
      return;
    if (previous == 0) {
      __atomic_store_n(&header->refs, 0, __ATOMIC_RELAXED);
      return;
    }
  }
  switch (header->tag) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_OBJECT: free(ref); break;
    /* Opaque framework handles are shared and remain valid for the process. */
    default: break; // TAG_ID / TAG_CLASS / UJ_TAG are pooled or externally owned
  }
}

static void delete_local(void *ref) {
  if (!ref)
    return;
  LocalState *state = local_state_current();
  if (!state)
    return;
  for (size_t i = state->top; i-- > 0;) {
    if (state->refs[i] == ref) {
      if (i + 1 < state->top)
        memmove(&state->refs[i], &state->refs[i + 1],
                (state->top - i - 1) * sizeof(*state->refs));
      state->top--;
      for (int frame = 0; frame < state->frame_top; frame++)
        if (state->frames[frame] > i)
          state->frames[frame]--;
      free_ref(ref);
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// object constructors
// ---------------------------------------------------------------------------

// Pool stateless service objects by class label.
#define MAX_IOBJ 512
static FakeObject iobj_pool[MAX_IOBJ];
static int iobj_count = 0;
void *jni_make_object(const char *label) {
  const char *l = (label && label[0]) ? label : "obj";
  mutexLock(&pool_lock);
  void *r = NULL;
  for (int i = 0; i < iobj_count; i++)
    if (!strcmp(iobj_pool[i].label, l)) { r = &iobj_pool[i]; break; }
  if (!r) {
    if (iobj_count >= MAX_IOBJ) r = &iobj_pool[0];
    else {
      FakeObject *o = &iobj_pool[iobj_count++];
      o->tag = TAG_CLASS;             // pooled: free_ref() ignores TAG_CLASS
      strncpy(o->label, l, sizeof(o->label) - 1);
      r = o;
    }
  }
  mutexUnlock(&pool_lock);
  return r;
}

void *jni_make_string(const char *utf) {
  const char *u = utf ? utf : "";
  mutexLock(&pool_lock);
  for (int i = 0; i < istr_count; i++)            // repeats reuse the pooled string
    if (!strcmp(istr_pool[i].utf, u)) { void *r = &istr_pool[i]; mutexUnlock(&pool_lock); return r; }
  if (istr_count < MAX_ISTR) {
    FakeString *s = &istr_pool[istr_count++];
    s->tag = TAG_STRING;
    s->refs = UINT32_MAX;
    s->utf = strdup(u);
    mutexUnlock(&pool_lock);
    return s;                                      // pooled, not reg_local'd
  }
  mutexUnlock(&pool_lock);
  FakeString *s = calloc(1, sizeof(*s));           // pool full: one-off local string
  s->tag = TAG_STRING;
  s->refs = 1;
  s->utf = strdup(u);
  return reg_local(s);
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->refs = 1;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "";
}

// Decode one UTF-8 code point. Invalid input is consumed one byte at a time and
// represented by U+FFFD, matching the non-throwing behaviour Unity expects from
// Java strings.
static uint32_t utf8_next(const unsigned char **cursor) {
  const unsigned char *p = *cursor;
  uint32_t cp;
  int count;

  if (p[0] < 0x80) {
    *cursor = p + 1;
    return p[0];
  }
  if ((p[0] & 0xe0) == 0xc0) {
    cp = p[0] & 0x1f;
    count = 2;
  } else if ((p[0] & 0xf0) == 0xe0) {
    cp = p[0] & 0x0f;
    count = 3;
  } else if ((p[0] & 0xf8) == 0xf0) {
    cp = p[0] & 0x07;
    count = 4;
  } else {
    *cursor = p + 1;
    return 0xfffd;
  }

  for (int i = 1; i < count; i++) {
    if (!p[i] || (p[i] & 0xc0) != 0x80) {
      *cursor = p + 1;
      return 0xfffd;
    }
    cp = (cp << 6) | (p[i] & 0x3f);
  }
  *cursor = p + count;

  const uint32_t minimum = count == 2 ? 0x80u : (count == 3 ? 0x800u : 0x10000u);
  if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
    return 0xfffd;
  return cp;
}

// Convert UTF-8 to Java UTF-16. When out is NULL this only counts code units.
static juint fake_utf8_to_utf16(const char *str, uint16_t *out) {
  const unsigned char *p = (const unsigned char *)(str ? str : "");
  juint n = 0;
  while (*p) {
    uint32_t cp = utf8_next(&p);
    if (cp < 0x10000) {
      if (out) out[n] = (uint16_t)cp;
      n++;
    } else {
      cp -= 0x10000;
      if (out) {
        out[n] = (uint16_t)(0xd800u + (cp >> 10));
        out[n + 1] = (uint16_t)(0xdc00u + (cp & 0x3ff));
      }
      n += 2;
    }
  }
  return n;
}

// UTF-16 code-unit count of a UTF-8 string (Java String.length()).
static juint utf16_len(const char *str) {
  return fake_utf8_to_utf16(str, NULL);
}

// ---------------------------------------------------------------------------
// interned classes + singletons
// ---------------------------------------------------------------------------

#define MAX_CLASSES 1024
static FakeClass class_pool[MAX_CLASSES];
static int class_count = 0;

static void *intern_class(const char *name) {
  for (int i = 0; i < class_count; i++)
    if (!strcmp(class_pool[i].name, name))
      return &class_pool[i];
  if (class_count >= MAX_CLASSES) {
    
    return &class_pool[0];
  }
  FakeClass *c = &class_pool[class_count++];
  c->tag = TAG_CLASS;
  strncpy(c->name, name, sizeof(c->name) - 1);
  
  return c;
}

static const char *class_name_of(void *cls) {
  FakeClass *c = cls;
  return (c && c->tag == TAG_CLASS) ? c->name : "";
}

static FakeObject *g_activity_obj = NULL;   // the MyNativeActivity instance
static FakeObject *g_asset_mgr = NULL;      // android.content.res.AssetManager

void *jni_make_activity_object(void) {
  if (!g_activity_obj) {
    g_activity_obj = calloc(1, sizeof(*g_activity_obj));
    g_activity_obj->tag = TAG_CLASS; // pooled (never freed)
    strcpy(g_activity_obj->label, "MyNativeActivity");
  }
  return g_activity_obj;
}

static void *get_asset_manager_obj(void) {
  if (!g_asset_mgr) {
    g_asset_mgr = calloc(1, sizeof(*g_asset_mgr));
    g_asset_mgr->tag = TAG_CLASS;
    strcpy(g_asset_mgr->label, "AssetManager");
  }
  return g_asset_mgr;
}

// Cached ClassLoader singleton.
static FakeObject *g_classloader = NULL;
static void *get_classloader_obj(void) {
  if (!g_classloader) {
    g_classloader = calloc(1, sizeof(*g_classloader));
    g_classloader->tag = TAG_CLASS;
    strcpy(g_classloader->label, "ClassLoader");
  }
  return g_classloader;
}

// ---------------------------------------------------------------------------
// method/field ID pool (class-aware)
// ---------------------------------------------------------------------------

#define MAX_IDS 8192
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *cls, const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig) &&
        !strcmp(id_pool[i].cls, cls))
      return &id_pool[i];
  if (id_count >= MAX_IDS) {
    
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->cls, cls ? cls : "", sizeof(id->cls) - 1);
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  return id;
}

// ---------------------------------------------------------------------------
// dispatch helpers
// ---------------------------------------------------------------------------

static int sig_returns(const char *sig, const char *ret) {
  const char *rp = strchr(sig, ')');
  return rp && strstr(rp + 1, ret) == rp + 1;
}

static int name_has(const char *name, const char *sub) { return strstr(name, sub) != NULL; }

// ---------------------------------------------------------------------------
// Offline Google Billing bridge (CTR2 / ZFramework 3)
// ---------------------------------------------------------------------------

static int offline_billing_method(const FakeID *id) {
  return id && name_has(id->cls, "com/zf3/billing/google/GoogleIapManager");
}

static int offline_billing_tag(void *object, uint32_t tag) {
  return object && *(uint32_t *)object == tag;
}

static int offline_billing_item_index(void *object) {
  const uintptr_t address = (uintptr_t)object;
  const uintptr_t products = (uintptr_t)&g_billing_products[0];
  const uintptr_t offers = (uintptr_t)&g_billing_offers[0];
  const size_t bytes = sizeof(g_billing_products);
  if (address >= products && address < products + bytes)
    return (int)((address - products) / sizeof(g_billing_products[0]));
  if (address >= offers && address < offers + bytes)
    return (int)((address - offers) / sizeof(g_billing_offers[0]));
  return -1;
}

static FakeBillingItem *offline_billing_product(const char *product_id) {
  const char *id = product_id ? product_id : "";
  for (unsigned i = 0; i < g_billing_item_count; ++i)
    if (!strcmp(g_billing_products[i].product_id, id))
      return &g_billing_products[i];
  if (g_billing_item_count >= OFFLINE_BILLING_MAX_PRODUCTS)
    return NULL;
  const unsigned i = g_billing_item_count++;
  g_billing_products[i].tag = TAG_BILLING_PRODUCT;
  g_billing_offers[i].tag = TAG_BILLING_OFFER;
  g_billing_lists[i].tag = TAG_BILLING_LIST;
  g_billing_lists[i].item = &g_billing_offers[i];
  snprintf(g_billing_products[i].product_id,
           sizeof(g_billing_products[i].product_id), "%.*s",
           OFFLINE_BILLING_ID_LENGTH - 1, id);
  snprintf(g_billing_offers[i].product_id,
           sizeof(g_billing_offers[i].product_id), "%.*s",
           OFFLINE_BILLING_ID_LENGTH - 1, id);
  return &g_billing_products[i];
}

static void offline_billing_catalogue_add(void *array_object) {
  FakeObjArray *array = array_object;
  if (!array || array->tag != TAG_OBJARR)
    return;
  for (int i = 0; i < array->len; ++i) {
    const char *id = obj_str(array->items[i]);
    if (!id || !id[0])
      continue;
    int duplicate = 0;
    for (unsigned j = 0; j < g_offline_billing.catalogue_count; ++j)
      if (!strcmp(g_offline_billing.catalogue[j], id)) {
        duplicate = 1;
        break;
      }
    if (duplicate ||
        g_offline_billing.catalogue_count >= OFFLINE_BILLING_MAX_PRODUCTS)
      continue;
    snprintf(g_offline_billing.catalogue[g_offline_billing.catalogue_count],
             OFFLINE_BILLING_ID_LENGTH, "%s", id);
    g_offline_billing.catalogue_count++;
  }
}

static void *offline_billing_object_common(void *receiver, const FakeID *id,
                                           void *object_arg, int int_arg,
                                           int *handled) {
  *handled = 0;
  if (!id)
    return NULL;

  if (offline_billing_method(id) &&
      !strcmp(id->name, "getBasePricingPhase")) {
    *handled = 1;
    const int index = offline_billing_item_index(object_arg);
    return index >= 0 ? &g_billing_offers[index] : NULL;
  }

  if (offline_billing_tag(receiver, TAG_BILLING_PRODUCT)) {
    const int index = offline_billing_item_index(receiver);
    *handled = 1;
    if (!strcmp(id->name, "getProductId"))
      return jni_make_string(((FakeBillingItem *)receiver)->product_id);
    if (!strcmp(id->name, "getDescription") ||
        !strcmp(id->name, "getName") || !strcmp(id->name, "getTitle"))
      return jni_make_string(((FakeBillingItem *)receiver)->product_id);
    if (!strcmp(id->name, "getProductType"))
      return jni_make_string("inapp");
    if (!strcmp(id->name, "getOneTimePurchaseOfferDetails"))
      return index >= 0 ? &g_billing_offers[index] : NULL;
    if (!strcmp(id->name, "getOneTimePurchaseOfferDetailsList") ||
        !strcmp(id->name, "getSubscriptionOfferDetails"))
      return index >= 0 ? &g_billing_lists[index] : NULL;
    return NULL;
  }

  if (offline_billing_tag(receiver, TAG_BILLING_OFFER)) {
    *handled = 1;
    if (!strcmp(id->name, "getFormattedPrice"))
      return jni_make_string("FREE");
    if (!strcmp(id->name, "getPriceCurrencyCode"))
      return jni_make_string("USD");
    if (!strcmp(id->name, "getOfferId") ||
        !strcmp(id->name, "getOfferToken") ||
        !strcmp(id->name, "getBillingPeriod"))
      return jni_make_string("");
    return NULL;
  }

  if (offline_billing_tag(receiver, TAG_BILLING_LIST)) {
    *handled = 1;
    if (!strcmp(id->name, "get") && int_arg == 0)
      return ((FakeBillingList *)receiver)->item;
    return NULL;
  }
  return NULL;
}

static juint offline_billing_int(void *receiver, const FakeID *id, int *handled) {
  *handled = 0;
  if (offline_billing_tag(receiver, TAG_BILLING_OFFER)) {
    if (!strcmp(id->name, "getPriceAmountMicros")) {
      *handled = 1;
      return 1;
    }
  }
  if (offline_billing_tag(receiver, TAG_BILLING_LIST)) {
    if (!strcmp(id->name, "size")) {
      *handled = 1;
      return 1;
    }
    if (!strcmp(id->name, "isEmpty")) {
      *handled = 1;
      return 0;
    }
  }
  return 0;
}

static int offline_billing_void_common(void *receiver, const FakeID *id,
                                       void *arg0, void *arg1, void *arg2) {
  if (!offline_billing_method(id))
    return 0;

  mutexLock(&g_offline_billing.lock);
  g_offline_billing.object = receiver;
  if (!strcmp(id->name, "init")) {
    g_offline_billing.pending_setup = 1;
    g_offline_billing.pending_locale = 1;
  } else if (!strcmp(id->name, "requestProductsData")) {
    g_offline_billing.catalogue_count = 0;
    offline_billing_catalogue_add(arg0);
    offline_billing_catalogue_add(arg1);
    offline_billing_catalogue_add(arg2);
    g_offline_billing.pending_products = 1;
  } else if (!strcmp(id->name, "purchase")) {
    snprintf(g_offline_billing.purchase_id,
             sizeof(g_offline_billing.purchase_id), "%s", obj_str(arg0));
    g_offline_billing.pending_purchase = 1;
  } else if (!strcmp(id->name, "queryPurchases")) {
    g_offline_billing.pending_restore = 1;
  } else if (!strcmp(id->name, "consume")) {
    snprintf(g_offline_billing.consume_id,
             sizeof(g_offline_billing.consume_id), "%s", obj_str(arg0));
    g_offline_billing.pending_consume = 1;
  } else if (!strcmp(id->name, "acknowledge")) {
    snprintf(g_offline_billing.acknowledge_id,
             sizeof(g_offline_billing.acknowledge_id), "%s", obj_str(arg0));
    g_offline_billing.pending_acknowledge = 1;
  }
  mutexUnlock(&g_offline_billing.lock);
  return 1;
}

static int offline_billing_void_a(void *receiver, const FakeID *id,
                                  const void *arguments) {
  if (!offline_billing_method(id))
    return 0;
  const FakeJValue *args = arguments;
  void *arg0 = NULL;
  void *arg1 = NULL;
  void *arg2 = NULL;
  if (args && !strcmp(id->name, "requestProductsData")) {
    arg0 = args[0].l;
    arg1 = args[1].l;
    arg2 = args[2].l;
  } else if (args && (!strcmp(id->name, "purchase") ||
                      !strcmp(id->name, "consume") ||
                      !strcmp(id->name, "acknowledge"))) {
    arg0 = args[0].l;
  }
  return offline_billing_void_common(receiver, id, arg0, arg1, arg2);
}

static const char *first_string_arg(const char *sig, va_list va);

static const char *lang_code(void) {
  if (config.language == LANG_JA) return "ja";
  if (config.language == LANG_EN) return "en";
  // LANG_AUTO: resolve the Switch system language once
  static const char *cached = NULL;
  if (!cached) {
    cached = "en";
    u64 code; SetLanguage sl;
    if (R_SUCCEEDED(setInitialize())) {
      if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &sl))) {
        switch (sl) {
          case SetLanguage_JA:                            cached = "ja"; break;
          case SetLanguage_FR: case SetLanguage_FRCA:     cached = "fr"; break;
          case SetLanguage_DE:                            cached = "de"; break;
          case SetLanguage_IT:                            cached = "it"; break;
          case SetLanguage_ES: case SetLanguage_ES419:    cached = "es"; break;
          case SetLanguage_PT: case SetLanguage_PTBR:     cached = "pt"; break;
          case SetLanguage_NL:                            cached = "nl"; break;
          case SetLanguage_RU:                            cached = "ru"; break;
          case SetLanguage_KO:                            cached = "ko"; break;
          case SetLanguage_ZHCN: case SetLanguage_ZHHANS:
          case SetLanguage_ZHTW: case SetLanguage_ZHHANT: cached = "zh"; break;
          default:                                        cached = "en"; break;
        }
      }
      setExit();
    }
  }
  return cached;
}

/* Locale getters consistent with lang_code() (libunity builds its culture string
 * as getLanguage()+"-"+getCountry()). */
typedef struct { const char *lang, *ctry, *iso3l, *iso3c, *loc; } LangRow;
static const LangRow *lang_row(void) {
  static const LangRow rows[] = {
    {"en","US","eng","USA","en_US"}, {"ja","JP","jpn","JPN","ja_JP"},
    {"fr","FR","fra","FRA","fr_FR"}, {"de","DE","deu","DEU","de_DE"},
    {"it","IT","ita","ITA","it_IT"}, {"es","ES","spa","ESP","es_ES"},
    {"pt","PT","por","PRT","pt_PT"}, {"nl","NL","nld","NLD","nl_NL"},
    {"ru","RU","rus","RUS","ru_RU"}, {"ko","KR","kor","KOR","ko_KR"},
    {"zh","CN","zho","CHN","zh_CN"},
  };
  const char *l = lang_code();
  for (unsigned i = 0; i < sizeof rows / sizeof *rows; i++)
    if (!strcmp(rows[i].lang, l)) return &rows[i];
  return &rows[0];
}

const char *jni_locale_name(void) {
  return lang_row()->loc;
}

/* Preserve the concrete class of the pooled Locale object. */
static int is_locale_object(const void *obj) {
  const uintptr_t p = (uintptr_t)obj;
  const uintptr_t first = (uintptr_t)&iobj_pool[0];
  const uintptr_t last = (uintptr_t)&iobj_pool[MAX_IOBJ];
  if (!obj || p < first || p >= last)
    return 0;
  const FakeObject *o = obj;
  return o->tag == TAG_CLASS && !strcmp(o->label, "java/util/Locale");
}

static void *locale_method_result(const char *method) {
  const LangRow *lr = lang_row();
  const char *value = NULL;
  if (!strcmp(method, "getLanguage"))          value = lr->lang;
  else if (!strcmp(method, "getCountry"))      value = lr->ctry;
  else if (!strcmp(method, "getISO3Language")) value = lr->iso3l;
  else if (!strcmp(method, "getISO3Country"))  value = lr->iso3c;
  else if (!strcmp(method, "toString") || name_has(method, "getDisplayName"))
    value = lr->loc;
  if (!strcmp(method, "toLanguageTag")) {
    static char tag[16];
    snprintf(tag, sizeof tag, "%s-%s", lr->lang, lr->ctry);
    value = tag;
  }
  if (value) {
    return jni_make_string(value);
  }
  return NULL;
}

// Return the first non-empty String argument in a JNI argument list.
static const char *first_string_arg(const char *sig, va_list va) {
  const char *p = sig ? strchr(sig, '(') : NULL;
  if (!p) return "";
  for (p++; *p && *p != ')'; p++) {
    switch (*p) {
      case 'I': case 'Z': case 'B': case 'C': case 'S': (void)va_arg(va, int); break;
      case 'F': case 'D': (void)va_arg(va, double); break;
      case 'J': (void)va_arg(va, long long); break;
      case '[':
        (void)va_arg(va, void *);
        if (p[1] == 'L') { p++; while (*p && *p != ';') p++; } else if (p[1]) p++;
        break;
      case 'L': {
        const char *s = obj_str(va_arg(va, void *));
        while (*p && *p != ';') p++;
        if (s && s[0]) return s;
        break;
      }
      default: break;
    }
  }
  return "";
}

/* Shared JNI string accessor. */
const char *jni_string_utf(void *jstr);

/* Return Switch audio parameters for FMOD's Android AudioManager query. */
static int g_last_output_prop = 0;

static void *getproperty_value(const char *key) {
  int which = 0;
  if (key && strstr(key, "SAMPLE_RATE"))            which = 1;
  else if (key && strstr(key, "FRAMES_PER_BUFFER")) which = 2;
  else                                              which = g_last_output_prop;
  if (which == 1) return jni_make_string("48000");   /* native output sample rate */
  if (which == 2) return jni_make_string("256");     /* native frames-per-buffer */
  return jni_make_string("");
}

// Preserve reflected method and field identity in FakeID.
static void *reflection_object_v(const FakeID *id, va_list va, int *handled) {
  *handled = 0;
  if (!name_has(id->cls, "unity3d/player/ReflectionHelper"))
    return NULL;

  if (!strcmp(id->name, "getConstructorID")) {
    void *target = va_arg(va, void *);
    const char *sig = obj_str(va_arg(va, void *));
    *handled = 1;
    return get_id(class_name_of(target), "<init>", sig);
  }
  if (!strcmp(id->name, "getMethodID") || !strcmp(id->name, "getFieldID")) {
    void *target = va_arg(va, void *);
    const char *name = obj_str(va_arg(va, void *));
    const char *sig = obj_str(va_arg(va, void *));
    (void)va_arg(va, int); // isStatic
    *handled = 1;
    return get_id(class_name_of(target), name, sig);
  }
  if (!strcmp(id->name, "getFieldSignature")) {
    FakeID *field = va_arg(va, void *);
    *handled = 1;
    return jni_make_string(field && field->tag == TAG_ID ? field->sig : "Ljava/lang/Object;");
  }
  if (!strcmp(id->name, "newProxyInstance") ||
      !strcmp(id->name, "createInvocationError")) {
    *handled = 1;
    return jni_make_object("com/unity3d/player/ReflectionHelper$Proxy");
  }
  return NULL;
}

static void *reflection_object_a(const FakeID *id, const void *args, int *handled) {
  *handled = 0;
  if (!name_has(id->cls, "unity3d/player/ReflectionHelper") || !args)
    return NULL;

  void *const *a = (void *const *)args;
  if (!strcmp(id->name, "getConstructorID")) {
    *handled = 1;
    return get_id(class_name_of(a[0]), "<init>", obj_str(a[1]));
  }
  if (!strcmp(id->name, "getMethodID") || !strcmp(id->name, "getFieldID")) {
    *handled = 1;
    return get_id(class_name_of(a[0]), obj_str(a[1]), obj_str(a[2]));
  }
  if (!strcmp(id->name, "getFieldSignature")) {
    FakeID *field = a[0];
    *handled = 1;
    return jni_make_string(field && field->tag == TAG_ID ? field->sig : "Ljava/lang/Object;");
  }
  if (!strcmp(id->name, "newProxyInstance") ||
      !strcmp(id->name, "createInvocationError")) {
    *handled = 1;
    return jni_make_object("com/unity3d/player/ReflectionHelper$Proxy");
  }
  return NULL;
}

/* ZJNIManager service objects remain typed by their map key. */
static void *zf_manager_service(void *key_obj) {
  const char *key = obj_str(key_obj);
  if (!key || !key[0]) key = class_name_of(key_obj);
  if (!key || !key[0]) key = "com/zf/ZService";
  const char *label = key;
  if (strstr(key, "Sound") || strstr(key, "sound")) label = "com/zf/ZSoundPlayer";
  else if (!strcmp(key,"videoPlayer")) label = "com/zf/ZVideoPlayer";
  else if (!strcmp(key,"eventDispatcher")) label = "com/zf/ZEventDispatcher";
  else if (!strcmp(key,"activities")) label = "com/zf/ZActivities";
  else if (!strcmp(key,"fontGeneratorFactory")) label = "com/zf/font/ZFontGeneratorFactory";
  else if (strstr(key, "Resource") || strstr(key, "resource")) label = "com/zf/ZResourceLoader";
  else if (strstr(key, "Device") || strstr(key, "device")) label = "com/zf/ZDeviceInfo";
  return jni_make_object(label);
}

static int zf_resource_owns(const FakeID *id) {
  return id && (strstr(id->cls, "ZResourceLoader") != NULL ||
                !strcmp(id->name, "filesDirectory"));
}

static int android_filesystem_owns(const FakeID *id) {
  return id && strstr(id->cls, "AndroidFileSystem") != NULL;
}

static int android_asset_path(char *out, size_t cap, const char *raw) {
  if (!out || !cap || !raw || strstr(raw, "..") || strchr(raw, ':')) return 0;
  while (*raw == '/') raw++;
  if (!strncmp(raw, "assets/", 7)) raw += 7;
  const int n = snprintf(out, cap, "%s/assets/%s", GAME_HOME, raw);
  return n > 0 && (size_t)n < cap;
}

static int android_asset_exists(const char *raw) {
  char path[768];
  struct stat st;
  if (!android_asset_path(path, sizeof path, raw)) return 0;
  return asset_pack_stat_path_info(path, NULL, NULL, NULL) ||
         (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

static void *j_NewObjectArray(void *env, int len, void *cls, void *init);

static int zf_resource_path(char *out, size_t cap, const char *raw) {
  if (!out || !cap || !raw || strstr(raw, "..")) return 0;
  const char *tail = raw;
  const char *root = GAME_HOME;
  if (!strncmp(tail, "assets:", 7)) { root = GAME_HOME "/assets"; tail += 7; }
  else if (!strncmp(tail, "internal:", 9)) { root = GAME_HOME "/files"; tail += 9; }
  else if (!strncmp(tail, "external:", 9)) { root = GAME_HOME "/files"; tail += 9; }
  else if (!strncmp(tail, "absolute:", 9)) { tail += 9; root = ""; }
  while (*tail == '/') tail++;
  if (!strncmp(raw, "sdmc:", 5) || raw[0] == '/')
    return snprintf(out, cap, "%s", raw) > 0;
  if (!strncmp(raw, "assets/", 7))
    return snprintf(out, cap, "%s/%s", GAME_HOME, raw) > 0;
  return snprintf(out, cap, "%s/%s", root, tail) > 0;
}

static int zf_path_exists(const char *path, int require_file) {
  uint64_t packed_size;
  int packed_directory = 0;
  if (asset_pack_stat_path_info(path, &packed_size, NULL, &packed_directory))
    return require_file ? !packed_directory : 1;
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  return require_file ? S_ISREG(st.st_mode) : 1;
}

/* Resolve plain ZResourceLoader paths through the APK asset directory. */
static int zf_resource_read_path(char *out, size_t cap, const char *raw,
                                 int require_file) {
  if (!zf_resource_path(out, cap, raw)) return 0;
  if (zf_path_exists(out, require_file)) return 1;
  if (!raw || raw[0] == '/' || strchr(raw, ':') || !strncmp(raw, "assets/", 7))
    return 1;
  return android_asset_path(out, cap, raw);
}

static int zf_resource_exists(const char *raw, char *resolved, size_t cap) {
  if (!zf_resource_read_path(resolved, cap, raw, 0)) return 0;
  return zf_path_exists(resolved, 0);
}

static void zf_mkdir_parents(const char *path) {
  char tmp[768]; snprintf(tmp, sizeof tmp, "%s", path ? path : "");
  char *start = strchr(tmp, ':'); start = start ? start + 2 : tmp + 1;
  for (char *p = start; *p; p++) if (*p == '/') { *p = 0; if (tmp[0]) mkdir(tmp, 0777); *p = '/'; }
}

static void *zf_resource_load(const char *raw) {
  char path[768]; if (!zf_resource_read_path(path, sizeof path, raw, 1)) return make_pri_array_adopt(malloc(1), 0, 1);
  void *packed_data = NULL; size_t packed_size = 0;
  if (asset_pack_read_all_path(path, &packed_data, &packed_size)) {
    if (packed_size > 0x7fffffff) { free(packed_data); return make_pri_array_adopt(malloc(1), 0, 1); }
    return make_pri_array_adopt(packed_data, (int)packed_size, 1);
  }
  FILE *f = fopen(path, "rb"); if (!f) return make_pri_array_adopt(malloc(1), 0, 1);
  fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
  if (size < 0 || size > 0x7fffffff) { fclose(f); return make_pri_array_adopt(malloc(1), 0, 1); }
  void *data = malloc(size ? (size_t)size : 1); if (size && fread(data, 1, (size_t)size, f) != (size_t)size) { free(data); data=malloc(1); size=0; }
  fclose(f);return make_pri_array_adopt(data, (int)size, 1);
}

static void zf_resource_save(void *array, const char *raw) {
  char path[768]; int size=0; void *data=jni_bytearray_data(array,&size); if(!data||!zf_resource_path(path,sizeof path,raw))return;
  zf_mkdir_parents(path);FILE *f=fopen(path,"wb");if(!f)return;if(size)fwrite(data,1,(size_t)size,f);fclose(f);
}

static void *zf_resource_files(const char *raw, const char *prefix, const char *suffix) {
  char path[768]; if(!zf_resource_read_path(path,sizeof path,raw,0))return j_NewObjectArray(NULL,0,NULL,NULL);
  DIR *dir=opendir(path); if(!dir)return j_NewObjectArray(NULL,0,NULL,NULL); char *names[512];int count=0;struct dirent *e;
  while(count<512&&(e=readdir(dir))){
    if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
    size_t n=strlen(e->d_name),plen=prefix?strlen(prefix):0,slen=suffix?strlen(suffix):0;
    if(plen&&strncmp(e->d_name,prefix,plen))continue;
    if(slen&&(n<slen||strcmp(e->d_name+n-slen,suffix)))continue;
    names[count++]=strdup(e->d_name);
  }
  closedir(dir);
  FakeObjArray *out=j_NewObjectArray(NULL,count,NULL,NULL);for(int i=0;i<count;i++){out->items[i]=jni_make_string(names[i]);free(names[i]);}return out;
}

/* Legacy ZPreferences bridge used by CTR2 progression. */
#if ZF_RENDERER_API == 1 || ZF_RENDERER_API == 2
static int ctr_prefs_factory(const FakeID *id){
  return id&&!strcmp(id->cls,"bk/bk/bi")&&!strcmp(id->name,"bk")&&
         !strcmp(id->sig,"()Ljava/lang/Object;");
}
static int ctr_prefs_method(const FakeID *id){
  return id&&!strcmp(id->cls,"bk/bk/bl");
}
static void *ctr_prefs_singleton(void){
  return jni_make_object("bk/bk/bl");
}
static void ctr_prefs_flush_if(int immediate){if(immediate)unity_jni_flush();}

static void *ctr_prefs_object_a(const FakeID *id,const void *args,int *handled){
  *handled=0;
  if(ctr_prefs_factory(id)){*handled=1;return ctr_prefs_singleton();}
  if(!ctr_prefs_method(id)||strcmp(id->name,"bo"))return NULL;
  const FakeJValue *a=args;const char *key=a?jni_string_utf(a[0].l):"";
  const char *fallback=strstr(id->sig,"Ljava/lang/String;Ljava/lang/String;")&&a?
                       jni_string_utf(a[1].l):"";
  *handled=1;return jni_make_string(unity_prefs_get_string(key,fallback));
}
static uint64_t ctr_prefs_int_a(const FakeID *id,const void *args,int *handled){
  *handled=0;if(!ctr_prefs_method(id))return 0;
  const FakeJValue *a=args;const char *key=a?jni_string_utf(a[0].l):"";
  if(!strcmp(id->name,"bn")){*handled=1;return unity_prefs_get_bool(key,a?a[1].z:0);}
  if(!strcmp(id->name,"bp")){*handled=1;return unity_prefs_has_key(key);}
  if(!strcmp(id->name,"bk")){*handled=1;return (uint64_t)unity_prefs_get_int64(key,strstr(id->sig,";I)")&&a?a[1].i:0);}
  if(!strcmp(id->name,"bl")){*handled=1;return (uint64_t)unity_prefs_get_int64(key,a?a[1].j:0);}
  return 0;
}
static float ctr_prefs_float_a(const FakeID *id,const void *args,int *handled){
  *handled=0;if(!ctr_prefs_method(id)||strcmp(id->name,"bm"))return 0.0f;
  const FakeJValue *a=args;*handled=1;
  return unity_prefs_get_float(a?jni_string_utf(a[0].l):"",a?a[1].f:0.0f);
}
static int ctr_prefs_void_a(const FakeID *id,const void *args){
  if(!ctr_prefs_method(id))return 0;
  const FakeJValue *a=args;
  if((!strcmp(id->name,"bk")&&!strcmp(id->sig,"()V"))||
     (!strcmp(id->name,"bl")&&!strcmp(id->sig,"()V"))||
     (!strcmp(id->name,"bm")&&!strcmp(id->sig,"()V"))){unity_jni_flush();return 1;}
  if(!strcmp(id->name,"bq"))return 1; /* native change callbacks: no Java peer */
  if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Z)V")){
    unity_prefs_clear();ctr_prefs_flush_if(a?a[0].z:0);return 1;
  }
  const char *key=a?jni_string_utf(a[0].l):"";
  if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Ljava/lang/String;Z)V")){
    unity_prefs_remove(key);ctr_prefs_flush_if(a?a[1].z:0);return 1;
  }
  if(!strcmp(id->name,"bl")&&!strcmp(id->sig,"(Ljava/lang/String;Z)V")){
    unity_prefs_remove_prefix(key);ctr_prefs_flush_if(a?a[1].z:0);return 1;
  }
  if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Ljava/lang/String;ZZ)V")){
    unity_prefs_set_bool(key,a?a[1].z:0);ctr_prefs_flush_if(a?a[2].z:0);return 1;
  }
  if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Ljava/lang/String;FZ)V")){
    unity_prefs_set_float(key,a?a[1].f:0.0f);ctr_prefs_flush_if(a?a[2].z:0);return 1;
  }
  if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Ljava/lang/String;IZ)V")){
    unity_prefs_set_int(key,a?a[1].i:0);ctr_prefs_flush_if(a?a[2].z:0);return 1;
  }
  if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Ljava/lang/String;JZ)V")){
    unity_prefs_set_int64(key,a?a[1].j:0);ctr_prefs_flush_if(a?a[2].z:0);return 1;
  }
  if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Ljava/lang/String;Ljava/lang/String;Z)V")){
    unity_prefs_set_string(key,a?jni_string_utf(a[1].l):"");ctr_prefs_flush_if(a?a[2].z:0);return 1;
  }
  return 0;
}
#else
static int ctr_prefs_factory(const FakeID *id){(void)id;return 0;}
static int ctr_prefs_method(const FakeID *id){(void)id;return 0;}
static void ctr_prefs_flush_if(int immediate){(void)immediate;}
static void *ctr_prefs_singleton(void){return NULL;}
static void *ctr_prefs_object_a(const FakeID *id,const void *args,int *handled){
  (void)id;(void)args;*handled=0;return NULL;
}
static uint64_t ctr_prefs_int_a(const FakeID *id,const void *args,int *handled){
  (void)id;(void)args;*handled=0;return 0;
}
static float ctr_prefs_float_a(const FakeID *id,const void *args,int *handled){
  (void)id;(void)args;*handled=0;return 0.0f;
}
static int ctr_prefs_void_a(const FakeID *id,const void *args){
  (void)id;(void)args;return 0;
}
#endif

/* ZF3 preferences use an isolated key prefix. */
#define ZF3_PREF_PREFIX "zf3_"

typedef void (*Zf3PrefBoolFn)(void *,void *,int64_t,void *,uint8_t);
typedef void (*Zf3PrefDoubleFn)(void *,void *,int64_t,void *,double);
typedef void (*Zf3PrefFloatFn)(void *,void *,int64_t,void *,float);
typedef void (*Zf3PrefIntFn)(void *,void *,int64_t,void *,int32_t);
typedef void (*Zf3PrefLongFn)(void *,void *,int64_t,void *,int64_t);
typedef void (*Zf3PrefStringFn)(void *,void *,int64_t,void *,void *);
static struct {
  void *clazz;
  Zf3PrefBoolFn on_bool;
  Zf3PrefDoubleFn on_double;
  Zf3PrefFloatFn on_float;
  Zf3PrefIntFn on_int;
  Zf3PrefLongFn on_long;
  Zf3PrefStringFn on_string;
} g_zf3_pref_callbacks;

static int zf3_prefs_method(const FakeID *id){
  return id&&!strcmp(id->cls,"com/zf3/preferences/AndroidPreferences");
}

static int zf3_pref_key(char out[320],const char *key){
  if(!key||strstr(key,".."))return 0;
  const int n=snprintf(out,320,ZF3_PREF_PREFIX "%s",key);
  return n>0&&n<320;
}

typedef struct { FakeObjArray *array;int index; } Zf3KeysContext;
static void zf3_collect_key(char type,const char *key,const char *value,void *opaque){
  (void)type;(void)value;Zf3KeysContext *context=opaque;
  if(!context||!context->array||context->index>=context->array->len)return;
  context->array->items[context->index++]=jni_make_string(key+strlen(ZF3_PREF_PREFIX));
}

static void *zf3_all_keys(void){
  size_t count=unity_prefs_count_prefix(ZF3_PREF_PREFIX);
  if(count>0x7fffffff)count=0x7fffffff;
  FakeObjArray *array=j_NewObjectArray(NULL,(int)count,NULL,NULL);
  Zf3KeysContext context={array,0};
  unity_prefs_visit_prefix(ZF3_PREF_PREFIX,zf3_collect_key,&context);
  return array;
}

typedef struct { int64_t visitor; } Zf3VisitContext;
static void zf3_emit_preference(char type,const char *key,const char *value,void *opaque){
  Zf3VisitContext *context=opaque;if(!context||!key||!value)return;
  void *clazz=g_zf3_pref_callbacks.clazz;
  void *jkey=jni_make_string(key+strlen(ZF3_PREF_PREFIX));
  switch(type){
    case 'B': if(g_zf3_pref_callbacks.on_bool)g_zf3_pref_callbacks.on_bool(fake_env,clazz,context->visitor,jkey,
                 value[0]=='1'||value[0]=='t'||value[0]=='T');break;
    case 'D': if(g_zf3_pref_callbacks.on_double)g_zf3_pref_callbacks.on_double(fake_env,clazz,context->visitor,jkey,strtod(value,NULL));break;
    case 'F': if(g_zf3_pref_callbacks.on_float)g_zf3_pref_callbacks.on_float(fake_env,clazz,context->visitor,jkey,(float)strtod(value,NULL));break;
    case 'I': if(g_zf3_pref_callbacks.on_int)g_zf3_pref_callbacks.on_int(fake_env,clazz,context->visitor,jkey,(int32_t)strtol(value,NULL,10));break;
    case 'L': if(g_zf3_pref_callbacks.on_long)g_zf3_pref_callbacks.on_long(fake_env,clazz,context->visitor,jkey,(int64_t)strtoll(value,NULL,10));break;
    case 'S': if(g_zf3_pref_callbacks.on_string)g_zf3_pref_callbacks.on_string(fake_env,clazz,context->visitor,jkey,jni_make_string(value));break;
    default:return;
  }
}

static void zf3_accept_visitor(int64_t visitor){
  Zf3VisitContext context={visitor};
  unity_prefs_visit_prefix(ZF3_PREF_PREFIX,zf3_emit_preference,&context);
}

static void *zf3_prefs_object_a(const FakeID *id,const void *args,int *handled){
  *handled=0;if(!zf3_prefs_method(id))return NULL;
  if(!strcmp(id->name,"allKeys")){*handled=1;return zf3_all_keys();}
  if(strcmp(id->name,"getString"))return NULL;
  const FakeJValue *a=args;const char *key=a?jni_string_utf(a[0].l):"";
  const char *fallback=a?jni_string_utf(a[1].l):"";char stored[320];
  *handled=1;return jni_make_string(zf3_pref_key(stored,key)?
                                    unity_prefs_get_string(stored,fallback):fallback);
}

static uint64_t zf3_prefs_int_a(const FakeID *id,const void *args,int *handled){
  *handled=0;if(!zf3_prefs_method(id))return 0;
  const FakeJValue *a=args;const char *key=a?jni_string_utf(a[0].l):"";char stored[320];
  if(!zf3_pref_key(stored,key))return 0;
  if(!strcmp(id->name,"hasKey")){*handled=1;return unity_prefs_has_key(stored);}
  if(!strcmp(id->name,"getBool")){*handled=1;return unity_prefs_get_bool(stored,a?a[1].z:0);}
  if(!strcmp(id->name,"getInt")){*handled=1;return (uint64_t)unity_prefs_get_int64(stored,a?a[1].i:0);}
  if(!strcmp(id->name,"getLong")){*handled=1;return (uint64_t)unity_prefs_get_int64(stored,a?a[1].j:0);}
  return 0;
}

static float zf3_prefs_float_a(const FakeID *id,const void *args,int *handled){
  *handled=0;if(!zf3_prefs_method(id)||strcmp(id->name,"getFloat"))return 0.0f;
  const FakeJValue *a=args;const char *key=a?jni_string_utf(a[0].l):"";char stored[320];
  *handled=1;return zf3_pref_key(stored,key)?unity_prefs_get_float(stored,a?a[1].f:0.0f):(a?a[1].f:0.0f);
}

static double zf3_prefs_double_a(const FakeID *id,const void *args,int *handled){
  *handled=0;if(!zf3_prefs_method(id)||strcmp(id->name,"getDouble"))return 0.0;
  const FakeJValue *a=args;const char *key=a?jni_string_utf(a[0].l):"";char stored[320];
  *handled=1;return zf3_pref_key(stored,key)?unity_prefs_get_double(stored,a?a[1].d:0.0):(a?a[1].d:0.0);
}

static int zf3_prefs_void_a(const FakeID *id,const void *args){
  if(!zf3_prefs_method(id))return 0;
  const FakeJValue *a=args;
  if(!strcmp(id->name,"commit")){unity_jni_flush();return 1;}
  if(!strcmp(id->name,"clear")){unity_prefs_remove_prefix(ZF3_PREF_PREFIX);return 1;}
  if(!strcmp(id->name,"acceptVisitor")){zf3_accept_visitor(a?a[0].j:0);return 1;}
  const char *key=a?jni_string_utf(a[0].l):"";char stored[320];
  if(!zf3_pref_key(stored,key))return 1;
  if(!strcmp(id->name,"erase")){unity_prefs_remove(stored);return 1;}
  if(!strcmp(id->name,"setBool")){unity_prefs_set_bool(stored,a?a[1].z:0);return 1;}
  if(!strcmp(id->name,"setDouble")){unity_prefs_set_double(stored,a?a[1].d:0.0);return 1;}
  if(!strcmp(id->name,"setFloat")){unity_prefs_set_float(stored,a?a[1].f:0.0f);return 1;}
  if(!strcmp(id->name,"setInt")){unity_prefs_set_int(stored,a?a[1].i:0);return 1;}
  if(!strcmp(id->name,"setLong")){unity_prefs_set_int64(stored,a?a[1].j:0);return 1;}
  if(!strcmp(id->name,"setString")){unity_prefs_set_string(stored,a?jni_string_utf(a[1].l):"");return 1;}
  return 1;
}

static void *act_object(const FakeID *id, va_list va) {
  if(!id)return NULL;
  int reflection_handled = 0;
  void *reflection_result = reflection_object_v(id, va, &reflection_handled);
  if (reflection_handled)
    return reflection_result;

  if(ctr_prefs_factory(id))return ctr_prefs_singleton();
  if(!strcmp(id->name,"getServiceInstance"))return jni_make_object(id->cls);
  if(ctr_prefs_method(id)&&!strcmp(id->name,"bo")){
    const char *key=obj_str(va_arg(va,void*));const char *fallback="";
    if(strstr(id->sig,"Ljava/lang/String;Ljava/lang/String;"))fallback=obj_str(va_arg(va,void*));
    return jni_make_string(unity_prefs_get_string(key,fallback));
  }
  if(zf3_prefs_method(id)){
    if(!strcmp(id->name,"allKeys"))return zf3_all_keys();
    if(!strcmp(id->name,"getString")){
      const char *key=obj_str(va_arg(va,void *));
      const char *fallback=obj_str(va_arg(va,void *));char stored[320];
      return jni_make_string(zf3_pref_key(stored,key)?
                             unity_prefs_get_string(stored,fallback):fallback);
    }
  }

  if (!strcmp(id->name, "get") &&
      !strcmp(id->sig, "(Ljava/lang/Object;)Ljava/lang/Object;"))
    return zf_manager_service(va_arg(va, void *));
  if (zf_resource_owns(id) && !strcmp(id->name, "loadData"))
    return zf_resource_load(obj_str(va_arg(va, void *)));
  if (zf_resource_owns(id) && !strcmp(id->name, "getFiles")) {
    const char *path=obj_str(va_arg(va,void*));const char *prefix=obj_str(va_arg(va,void*));const char *suffix=obj_str(va_arg(va,void*));
    return zf_resource_files(path,prefix,suffix);
  }

  /* Use the renderer loop instead of Android Choreographer. */
  if ((name_has(id->cls, "Choreographer") || name_has(id->cls, "SwappyDisplayManager")) &&
      (name_has(id->name, "getInstance") || sig_returns(id->sig, "Landroid/view/Choreographer;")))
    return NULL;
  /* PlayerPrefs URI encoding is an identity operation on this host. */
  if ((name_has(id->cls, "net/Uri") || sig_returns(id->sig,"Ljava/lang/String;")) &&
      (!strcmp(id->name, "encode") || !strcmp(id->name, "decode"))) {
    void *arg = va_arg(va, void *);
    /* Skip a leading cached jclass argument. */
    if (arg && *(uint32_t *)arg == TAG_CLASS) arg = va_arg(va, void *);
    return arg;
  }
  /* Supply the AssetManager object used by AndroidFileSystem. */
  if (!strcmp(id->name, "assetManager") || name_has(id->name, "AssetManager") ||
      sig_returns(id->sig, "Landroid/content/res/AssetManager;")) {
    return get_asset_manager_obj();
  }
  if (name_has(id->name, "ClassLoader") || sig_returns(id->sig, "Ljava/lang/ClassLoader;"))
    return get_classloader_obj();
  /* Non-null no-op Android service handle. */
  if (!strcmp(id->name, "getSystemService"))
    return jni_make_object("android/os/Vibrator");
  /* Non-null notification manager singleton. */
  if (!strcmp(id->name, "getNotificationManagerImpl"))
    return jni_make_object("com/unity/androidnotifications/UnityNotificationManager");
  if (sig_returns(id->sig, "Ljava/lang/Class;"))
    return intern_class("java/lang/Object");
  // version / package / device / storage strings
  if (name_has(id->name, "VersionName")) return jni_make_string(GAME_VERSION_NAME);
  if (!strcmp(id->name, "getAppVersion")) return jni_make_string(GAME_VERSION_NAME);
  if (!strcmp(id->name, "getAppName")) return jni_make_string(GAME_TITLE);
  if (name_has(id->name, "PackageName")) return jni_make_string(GAME_PACKAGE);
  if (name_has(id->name, "DeviceModel")) return jni_make_string("Switch");
  if (!strcmp(id->name, "getModel")) return jni_make_string("Switch");
  if (!strcmp(id->name, "getDeviceManufacturer")) return jni_make_string("Nintendo");
  if (!strcmp(id->name, "getOSVersionAsString")) return jni_make_string("13");
  if (!strcmp(id->name, "getAndroidId")) return jni_make_string("nintendo-switch");
  if (!strcmp(id->name, "getCountryISOCode")) return jni_make_string(lang_row()->ctry);
  if (!strcmp(id->name, "getLocale") || !strcmp(id->name, "getLocaleStatic")) return jni_make_string(lang_row()->loc);
  if (!strcmp(id->name, "getTimeZone")) return jni_make_string("UTC");
  if (name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(va_arg(va, void *)));
  if (name_has(id->name, "Language") || name_has(id->name, "language"))
    return jni_make_string(lang_code());
  // Android external storage is mounted.
  if (name_has(id->cls, "os/Environment")) {
    if (name_has(id->name, "ExternalStorageState")) return jni_make_string("mounted");
    if (name_has(id->name, "Directory")) return jni_make_object("java/io/File"); /* ->getAbsolutePath */
  }
  // Locale.getDefault() is exposed through a generic Object signature.
  if (name_has(id->cls, "Locale")) {
    if (!strcmp(id->name, "getDefault")) {
      return jni_make_object("java/util/Locale");
    }
    void *result = locale_method_result(id->name);
    if (result) return result;
  }
  if (!strcmp(id->name, "getPackageCodePath") ||
      !strcmp(id->name, "getPackageResourcePath"))
    return jni_make_string(managed_path(GAME_HOME "/assets.apk"));
  if (!strcmp(id->name, "cachesDirectory")) {
    mkdir(GAME_HOME "/cache", 0777);
    return jni_make_string(managed_path(GAME_HOME "/cache"));
  }
  if (zf_resource_owns(id) && !strcmp(id->name,"getCachePath")) {
    mkdir(GAME_HOME "/cache",0777);
    return jni_make_string(managed_path(GAME_HOME "/cache"));
  }
  if (zf_resource_owns(id) && !strcmp(id->name,"getInternalStoragePath")) {
    mkdir(GAME_HOME "/files",0777);
    return jni_make_string(managed_path(GAME_HOME "/files"));
  }
  if (android_filesystem_owns(id) && !strcmp(id->name,"filesDirectory")) {
    mkdir(GAME_HOME "/files",0777);
    return jni_make_string(managed_path(GAME_HOME "/files"));
  }
  if (name_has(id->name, "DataPath") || name_has(id->name, "StoragePath") ||
      name_has(id->name, "FilesDir") || name_has(id->name, "RootPath") ||
      name_has(id->name, "ObbDir") || name_has(id->name, "AssetPath") ||
      name_has(id->name, "Path") || !strcmp(id->name, "filesDirectory") ||
      !strcmp(id->name, "cacheDirectory"))
    return jni_make_string(managed_path(GAME_HOME));
  // Beacon expects a JSON object.
  if (!strcmp(id->name, "getPlatformIdentifiers"))
    return jni_make_string("{\"installSource\":\"\",\"androidId\":\"\",\"buildId\":\"Switch\",\"isChromeOS\":false}");
  if (!strcmp(id->name, "getMetaInstallReferrer"))
    return jni_make_string("");
  if (name_has(id->cls, "beacon/Localization"))
    return jni_make_string(lang_row()->loc);
  if (name_has(id->cls, "beacon/DeviceInfo")) {
    if (name_has(id->name, "Locale") || name_has(id->name, "locale"))
      return jni_make_string(lang_row()->loc);
    if (sig_returns(id->sig, "Ljava/lang/String;"))
      return jni_make_string("");
  }
  // Package metadata objects.
  if (name_has(id->name, "getPackageInfo"))     return jni_make_object("android/content/pm/PackageInfo");
  if (name_has(id->name, "getApplicationInfo")) return jni_make_object("android/content/pm/ApplicationInfo");
  if (name_has(id->name, "getPackageManager"))  return jni_make_object("android/content/pm/PackageManager");
  if (name_has(id->name, "getResources"))       return jni_make_object("android/content/res/Resources");
  if (name_has(id->name, "getConfiguration"))   return jni_make_object("android/content/res/Configuration");
  // Typed Locale return.
  if (sig_returns(id->sig, "Ljava/util/Locale;"))
    return jni_make_object("java/util/Locale");
  /* Return a valid empty byte array for unsupported serializers. */
  if (sig_returns(id->sig, "[B")) {
    return make_pri_array_adopt(malloc(1), 0, 1);
  }
  if (sig_returns(id->sig, "Ljava/lang/String;"))
    return jni_make_string(""); // UUID, asset-pack name, etc.
  (void)va;
  return NULL;
}

static juint act_int(const FakeID *id, va_list va) {
  if(zf3_prefs_method(id)){
    const char *key=obj_str(va_arg(va,void *));char stored[320];
    if(!zf3_pref_key(stored,key))return 0;
    if(!strcmp(id->name,"hasKey"))return unity_prefs_has_key(stored);
    if(!strcmp(id->name,"getBool"))return unity_prefs_get_bool(stored,va_arg(va,int));
    if(!strcmp(id->name,"getInt"))return (juint)unity_prefs_get_int64(stored,va_arg(va,int));
    if(!strcmp(id->name,"getLong"))return (juint)unity_prefs_get_int64(stored,va_arg(va,long long));
  }
  if(ctr_prefs_method(id)){
    const char *key=obj_str(va_arg(va,void*));
    if(!strcmp(id->name,"bn"))return unity_prefs_get_bool(key,va_arg(va,int));
    if(!strcmp(id->name,"bp"))return unity_prefs_has_key(key);
    if(!strcmp(id->name,"bk")){
      int fallback=strstr(id->sig,";I)")?va_arg(va,int):0;
      return (juint)unity_prefs_get_int64(key,fallback);
    }
    if(!strcmp(id->name,"bl"))return (juint)unity_prefs_get_int64(key,va_arg(va,long long));
  }
  if (zf_audio_handles(id->cls, id->name))
    return zf_audio_int_v(id->name, id->sig, va);
  if (zf_resource_owns(id) && !strcmp(id->name,"isFileExists")) {
    char path[768];const char *raw=obj_str(va_arg(va,void*));
    const int exists=zf_resource_exists(raw,path,sizeof path);
    return (juint)exists;
  }
  if (android_filesystem_owns(id) && !strcmp(id->name, "assetExists")) {
    const char *raw = obj_str(va_arg(va, void *));
    const int exists = android_asset_exists(raw);
    return (juint)exists;
  }
  if (android_filesystem_owns(id) && !strcmp(id->name,"fileExists")) {
    const char *raw=obj_str(va_arg(va,void *));struct stat st;
    const int exists=raw&&raw[0]&&!strstr(raw,"..")&&stat(raw,&st)==0;
    return (juint)exists;
  }
  if (zf_resource_owns(id) && !strcmp(id->name,"getFreeSpace")) return 0x40000000u;
  if (!strcmp(id->name,"getOSVersionAsInt")) return 33;
  if (!strcmp(id->name,"totalMemory")) return 2048;
  if (!strcmp(id->name,"isSignatureMatches")) return 1;
  /* The local billing service satisfies CTR2's store connectivity gate. */
  if (name_has(id->cls, "com/zf/modules/device/ZSystemInfo")) {
    if (!strcmp(id->name, "isNetworkAvailable")) return 1;
    if (!strcmp(id->name, "getNetworkType")) return 1;
  }
  // Numeric parsing used by Android audio properties.
  if (name_has(id->name, "parseInt") || name_has(id->name, "parseLong")) {
    const char *s = first_string_arg(id->sig, va);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    
    return v;
  }
  if (name_has(id->name, "playCoreApiMissing")) return 1;
  // Google Play Services is unavailable on Switch.
  if (name_has(id->name, "isGooglePlayServicesAvailable")) return 1; /* ConnectionResult.SERVICE_MISSING */
  // Android permissions are provided by the host environment.
  if (name_has(id->name, "checkPermission")     || name_has(id->name, "hasPermission") ||
      name_has(id->name, "isPermissionGranted") || name_has(id->name, "checkNotificationStatus"))
    return 1;
  if (name_has(id->name, "shouldShowRequestPermissionRationale")) return 0;
  (void)va;
  // Unsupported boolean queries default to false.
  return 0;
}

static float act_float(const FakeID *id, va_list va) {
  if(zf3_prefs_method(id)&&!strcmp(id->name,"getFloat")){
    const char *key=obj_str(va_arg(va,void *));const float fallback=(float)va_arg(va,double);
    char stored[320];return zf3_pref_key(stored,key)?unity_prefs_get_float(stored,fallback):fallback;
  }
  if(ctr_prefs_method(id)&&!strcmp(id->name,"bm")){
    const char *key=obj_str(va_arg(va,void*));
    return unity_prefs_get_float(key,(float)va_arg(va,double));
  }
  if (zf_audio_handles(id->cls, id->name))
    return zf_audio_float_v(id->name, id->sig, va);
  (void)va;
  float x, y, z;
  android_get_orientation(&x, &y, &z);
  if (name_has(id->name, "OrientationX")) return x;
  if (name_has(id->name, "OrientationY")) return y;
  if (name_has(id->name, "OrientationZ")) return z;
  if (!strcmp(id->name,"getDensity") || !strcmp(id->name,"getDensityMagic")) return 1.0f;
  return 0.0f;
}

static double act_double(const FakeID *id,va_list va){
  if(zf3_prefs_method(id)&&!strcmp(id->name,"getDouble")){
    const char *key=obj_str(va_arg(va,void *));const double fallback=va_arg(va,double);
    char stored[320];return zf3_pref_key(stored,key)?unity_prefs_get_double(stored,fallback):fallback;
  }
  (void)id;(void)va;return 0.0;
}

static int is_video_play_method(const FakeID *id) {
  return id && !strcmp(id->name,"playVideo") &&
         (!strcmp(id->cls,"videoPlayer") || name_has(id->cls,"ZVideoPlayer"));
}

static void queue_video_playback(void *path_ref,int flag1,int flag2) {
  const char *path=obj_str(path_ref);
  ctr_video_request(path,flag1,flag2);
}

static int zf3_threads_void(const FakeID *id){
  if(!id||strcmp(id->cls,"com/zf3/threads/AndroidThreadManager"))return 0;
  if(!strcmp(id->name,"requestUICallback")){
    __atomic_store_n(&g_zf3_threads.pending,1,__ATOMIC_RELEASE);
    return 1;
  }
  if(!strcmp(id->name,"cleanup")){
    __atomic_store_n(&g_zf3_threads.pending,0,__ATOMIC_RELEASE);
    g_zf3_threads.native_handle=0;
    return 1;
  }
  return 0;
}

static void act_void(const FakeID *id, va_list va) {
  if(zf3_threads_void(id))return;
  if(zf3_prefs_method(id)){
    if(!strcmp(id->name,"commit")){unity_jni_flush();return;}
    if(!strcmp(id->name,"clear")){unity_prefs_remove_prefix(ZF3_PREF_PREFIX);return;}
    if(!strcmp(id->name,"acceptVisitor")){zf3_accept_visitor(va_arg(va,long long));return;}
    const char *key=obj_str(va_arg(va,void *));char stored[320];
    if(!zf3_pref_key(stored,key))return;
    if(!strcmp(id->name,"erase")){unity_prefs_remove(stored);return;}
    if(!strcmp(id->name,"setBool")){unity_prefs_set_bool(stored,va_arg(va,int));return;}
    if(!strcmp(id->name,"setDouble")){unity_prefs_set_double(stored,va_arg(va,double));return;}
    if(!strcmp(id->name,"setFloat")){unity_prefs_set_float(stored,(float)va_arg(va,double));return;}
    if(!strcmp(id->name,"setInt")){unity_prefs_set_int(stored,va_arg(va,int));return;}
    if(!strcmp(id->name,"setLong")){unity_prefs_set_int64(stored,va_arg(va,long long));return;}
    if(!strcmp(id->name,"setString")){unity_prefs_set_string(stored,obj_str(va_arg(va,void *)));return;}
    return;
  }
  if(ctr_prefs_method(id)){
    if((!strcmp(id->name,"bk")&&!strcmp(id->sig,"()V"))||
       (!strcmp(id->name,"bl")&&!strcmp(id->sig,"()V"))||
       (!strcmp(id->name,"bm")&&!strcmp(id->sig,"()V"))){unity_jni_flush();return;}
    if(!strcmp(id->name,"bq"))return;
    if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Z)V")){
      int immediate=va_arg(va,int);unity_prefs_clear();ctr_prefs_flush_if(immediate);return;
    }
    const char *key=obj_str(va_arg(va,void*));
    if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Ljava/lang/String;Z)V")){
      int immediate=va_arg(va,int);unity_prefs_remove(key);ctr_prefs_flush_if(immediate);return;
    }
    if(!strcmp(id->name,"bl")&&!strcmp(id->sig,"(Ljava/lang/String;Z)V")){
      int immediate=va_arg(va,int);unity_prefs_remove_prefix(key);ctr_prefs_flush_if(immediate);return;
    }
    if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Ljava/lang/String;ZZ)V")){
      int value=va_arg(va,int),immediate=va_arg(va,int);unity_prefs_set_bool(key,value);ctr_prefs_flush_if(immediate);return;
    }
    if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Ljava/lang/String;FZ)V")){
      float value=(float)va_arg(va,double);int immediate=va_arg(va,int);unity_prefs_set_float(key,value);ctr_prefs_flush_if(immediate);return;
    }
    if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Ljava/lang/String;IZ)V")){
      int value=va_arg(va,int),immediate=va_arg(va,int);unity_prefs_set_int(key,value);ctr_prefs_flush_if(immediate);return;
    }
    if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Ljava/lang/String;JZ)V")){
      long long value=va_arg(va,long long);int immediate=va_arg(va,int);unity_prefs_set_int64(key,value);ctr_prefs_flush_if(immediate);return;
    }
    if(!strcmp(id->name,"bk")&&!strcmp(id->sig,"(Ljava/lang/String;Ljava/lang/String;Z)V")){
      const char *value=obj_str(va_arg(va,void*));int immediate=va_arg(va,int);unity_prefs_set_string(key,value);ctr_prefs_flush_if(immediate);return;
    }
  }
  if (zf_audio_handles(id->cls, id->name)) {
    zf_audio_void_v(id->name, id->sig, va);
    return;
  }
  if (zf_resource_owns(id)) {
    if (!strcmp(id->name,"saveData")) { void *data=va_arg(va,void*);zf_resource_save(data,obj_str(va_arg(va,void*)));return; }
    if (!strcmp(id->name,"createFolder")) { char path[768];if(zf_resource_path(path,sizeof path,obj_str(va_arg(va,void*)))){zf_mkdir_parents(path);mkdir(path,0777);}return; }
    if (!strcmp(id->name,"remove")||!strcmp(id->name,"removeFolder")) { char path[768];if(zf_resource_path(path,sizeof path,obj_str(va_arg(va,void*)))){unlink(path);rmdir(path);}return; }
  }
  if (is_video_play_method(id)) {
    void *path=va_arg(va,void *);
    const int flag1=va_arg(va,int),flag2=va_arg(va,int);
    queue_video_playback(path,flag1,flag2);
    return;
  }
  if (!strcmp(id->name, "showSoftInput")) {
    const char *initial = obj_str(va_arg(va, void *));
    const int keyboard_type = va_arg(va, int);
    (void)va_arg(va, int); /* autocorrection */
    (void)va_arg(va, int); /* multiline */
    const int secure = va_arg(va, int);
    (void)va_arg(va, int); /* alert */
    const char *guide = obj_str(va_arg(va, void *));
    int character_limit = va_arg(va, int);
    (void)va_arg(va, int); /* hide input field */
    if (character_limit <= 0 || character_limit > 500)
      character_limit = 500;

    mutexLock(&g_soft_keyboard.lock);
    snprintf(g_soft_keyboard.initial, sizeof g_soft_keyboard.initial,
             "%s", initial ? initial : "");
    snprintf(g_soft_keyboard.guide, sizeof g_soft_keyboard.guide,
             "%s", guide ? guide : "");
    g_soft_keyboard.keyboard_type = keyboard_type;
    g_soft_keyboard.secure = secure != 0;
    g_soft_keyboard.character_limit = character_limit;
    g_soft_keyboard.pending = 1;
    mutexUnlock(&g_soft_keyboard.lock);
    return;
  }
  if (!strcmp(id->name, "finish") || !strcmp(id->name,"exit") || name_has(id->name, "appEnd") ||
      name_has(id->name, "exitApp"))
    jni_quit_requested = 1;
  // openStore / sendBroadcast / IME open / Mobage / web view: no-op
}

// ---------------------------------------------------------------------------
// top-level dispatch by class + return kind
// ---------------------------------------------------------------------------

/* Delegate Unity and input classes to their stateful handlers. */
#include "unity_input.h"

static void *dispatch_object(void *recv, const FakeID *id, va_list va) {
  int billing_handled = 0;
  void *billing_object_arg = NULL;
  int billing_int_arg = 0;
  if (offline_billing_method(id) && !strcmp(id->name, "getBasePricingPhase"))
    billing_object_arg = va_arg(va, void *);
  else if (offline_billing_tag(recv, TAG_BILLING_LIST) &&
           !strcmp(id->name, "get"))
    billing_int_arg = va_arg(va, int);
  void *billing_result = offline_billing_object_common(
      recv, id, billing_object_arg, billing_int_arg, &billing_handled);
  if (billing_handled)
    return billing_result;
  /* Preserve pooled object types through Object.getClass(). */
  if (recv && !strcmp(id->name, "getClass")) {
    const char *name = class_name_of(recv);
    return intern_class(name && name[0] ? name : "java/lang/Object");
  }
  /* Copy MotionEvent handles. */
  if (!strcmp(id->name, "obtain") &&
      strstr(id->sig, "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;")) {
    void *src = va_arg(va, void *);
    return unity_motionevent_obtain(src);
  }
  /* String.getBytes() returns UTF-8 data. */
  if (recv && *(uint32_t *)recv == TAG_STRING && name_has(id->name, "getBytes")) {
    const char *u = ((FakeString *)recv)->utf; int n = (int)strlen(u);
    char *d = malloc(n > 0 ? n : 1); if (n) memcpy(d, u, n);
    return make_pri_array_adopt(d, n, 1);
  }
  /* Load ZResourceLoader byte arrays before the generic array handler. */
  if (zf_resource_owns(id) && !strcmp(id->name, "loadData"))
    return zf_resource_load(obj_str(va_arg(va, void *)));
  /* Unsupported byte-array methods return an empty array. */
  if (sig_returns(id->sig, "[B")) {
    return make_pri_array_adopt(malloc(1), 0, 1);
  }
  /* Route Locale methods by receiver identity. */
  if (is_locale_object(recv)) {
    void *result = locale_method_result(id->name);
    if (result) return result;
  }
  // Reflected Method/Field handles retain their declaring class in FakeID.
  if (recv && *(uint32_t *)recv == TAG_ID && !strcmp(id->name, "getDeclaringClass"))
    return intern_class(((FakeID *)recv)->cls);
  if (unity_owns_recv(recv) || !strcmp(id->name,"getSharedPreferences") ||
      unity_owns_class(id->cls))
    return unity_dispatch_object(recv, id, va);
  return act_object(id, va);
}
static juint dispatch_int(void *recv, const FakeID *id, va_list va) {
  int billing_handled = 0;
  const juint billing_result = offline_billing_int(recv, id, &billing_handled);
  if (billing_handled)
    return billing_result;
  // String integer methods are routed by receiver tag.
  if (recv && *(uint32_t *)recv == TAG_STRING) {
    if (!strcmp(id->name, "length"))   return utf16_len(((FakeString *)recv)->utf);
    if (!strcmp(id->name, "hashCode")) return 0;
    if (!strcmp(id->name, "isEmpty"))  return ((FakeString *)recv)->utf[0] == '\0';
  }
  /* Unbox PlayerPrefs values. */
  if (unity_is_boxed(recv)) return unity_boxed_int(recv);
  /* Route input getters by receiver tag. */
  if (input_owns_recv(recv)) return input_dispatch_int(recv, id, va);
  if (unity_owns_recv(recv) || unity_owns_class(id->cls)) return unity_dispatch_int(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_int(recv, id, va);
  return act_int(id, va);
}
static float dispatch_float(void *recv, const FakeID *id, va_list va) {
  if (unity_is_boxed(recv)) return unity_boxed_float(recv);   /* Float.floatValue */
  if (unity_owns_recv(recv) || unity_owns_class(id->cls)) return unity_dispatch_float(recv,id,va);
  if (input_owns_recv(recv)) return input_dispatch_float(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_float(recv, id, va);
  return act_float(id, va);
}
static double dispatch_double(void *recv,const FakeID *id,va_list va){
  (void)recv;return act_double(id,va);
}
static void dispatch_void(void *recv, const FakeID *id, va_list va) {
  if (offline_billing_method(id)) {
    void *arg0 = NULL;
    void *arg1 = NULL;
    void *arg2 = NULL;
    if (!strcmp(id->name, "requestProductsData")) {
      arg0 = va_arg(va, void *);
      arg1 = va_arg(va, void *);
      arg2 = va_arg(va, void *);
    } else if (!strcmp(id->name, "purchase") ||
               !strcmp(id->name, "consume") ||
               !strcmp(id->name, "acknowledge")) {
      arg0 = va_arg(va, void *);
    }
    offline_billing_void_common(recv, id, arg0, arg1, arg2);
    return;
  }
  if (unity_owns_recv(recv) || unity_owns_class(id->cls)) { unity_dispatch_void(recv, id, va); return; }
  act_void(id, va);
}

// ---------------------------------------------------------------------------
// JNIEnv function implementations
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) {
  (void)env;
  return intern_class(name ? name : "?");
}
static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;
  if (obj && *(uint32_t *)obj == TAG_STRING)
    return intern_class("java/lang/String");
  if (offline_billing_tag(obj, TAG_BILLING_PRODUCT))
    return intern_class("com/android/billingclient/api/ProductDetails");
  if (offline_billing_tag(obj, TAG_BILLING_OFFER))
    return intern_class("com/android/billingclient/api/ProductDetails$OneTimePurchaseOfferDetails");
  if (offline_billing_tag(obj, TAG_BILLING_LIST))
    return intern_class("java/util/List");
  /* Preserve the concrete input-event class. */
  if (input_owns_recv(obj))
    return intern_class(input_recv_is_motion(obj)
                        ? "android/view/MotionEvent"
                        : "android/view/KeyEvent");
  /* Preserve the Locale class for subsequent method resolution. */
  if (is_locale_object(obj)) return intern_class("java/util/Locale");
  const char *uclass=unity_recv_class(obj);
  if (uclass) return intern_class(uclass);
  /* Preserve labelled ZFramework service classes. */
  if (obj && *(uint32_t *)obj == TAG_CLASS) {
    const char *name = class_name_of(obj);
    if (name && name[0]) return intern_class(name);
  }
  return intern_class("java/lang/Object");
}
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}
static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}

/* Decode Java String byte-array constructors as UTF-8. */
static void *new_object_dispatch(void *cls, void *mid, void *first_arg) {
  const char *cn = class_name_of(cls);
  FakeID *m = mid;
  /* Accept the byte-array constructor even when resolved on Object. */
  const int byte_string_ctor = m && !strcmp(m->name, "<init>") &&
                               !strcmp(m->sig, "([BLjava/lang/String;)V");
  if ((cn && strstr(cn, "java/lang/String")) || byte_string_ctor) {
    if (m && strstr(m->sig, "[B")) {              /* String([B...) */
      int len = 0; char *b = jni_bytearray_data(first_arg, &len);
      if (b && len > 0) { char *t = malloc(len + 1); memcpy(t, b, len); t[len] = 0;
        void *s = jni_make_string(t); free(t); return s; }
      return jni_make_string("");
    }
  }
  void *object=jni_make_object(cn);
  if(cn&&!strcmp(cn,"com/zf3/threads/AndroidThreadManager")){
    /* Retain the manager for deferred callbacks. */
    retain_ref(object);
    g_zf3_threads.object=object;
    g_zf3_threads.native_handle=(int64_t)(intptr_t)first_arg;
  }
  return object;
}

static void *j_NewObject(void *env, void *cls, void *mid, ...) {
  (void)env;
  FakeID *method=mid;void *a0=NULL;va_list va;va_start(va,mid);
  if(method&&method->sig[0]=='('&&method->sig[1]!=')')a0=va_arg(va,void *);
  va_end(va);
  return new_object_dispatch(cls, mid, a0);
}
static void *j_NewObjectV(void *env, void *cls, void *mid, va_list va) {
  (void)env;FakeID *method=mid;void *a0=NULL;
  if(method&&method->sig[0]=='('&&method->sig[1]!=')')a0=va_arg(va,void *);
  return new_object_dispatch(cls, mid, a0);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  /* NewGlobalRef retains the caller's local reference. */
  retain_ref(obj);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) {
  (void)env;
  retain_ref(obj);
  return reg_local(obj);
}
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }

/* IsInstanceOf with exact handling for input and boxed values. */
static juint j_IsInstanceOf(void *env, void *obj, void *clazz) {
  (void)env;
  const char *cn = class_name_of(clazz);
  /* Classify input handles by their concrete kind. */
  if (input_owns_recv(obj)) {
    if (strstr(cn, "MotionEvent")) return input_recv_is_motion(obj) ? 1 : 0;
    if (strstr(cn, "KeyEvent"))    return input_recv_is_motion(obj) ? 0 : 1;
    if (strstr(cn, "InputEvent"))  return 1;
    /* Resolve untyped input handles by the requested event class. */
    const juint answer=(juint)input_instanceof_untyped(obj);
    return answer;
  }
  /* Match boxed PlayerPrefs values exactly. */
  int ui = unity_isinstance(obj, cn);
  if (ui >= 0) return (juint)ui;
  if (obj && *(uint32_t *)obj == TAG_STRING) {
    if (strstr(cn, "String")) return 1;
    if (strstr(cn, "Integer") || strstr(cn, "Long") || strstr(cn, "Float") ||
        strstr(cn, "Double")  || strstr(cn, "Boolean") || strstr(cn, "Character") ||
        strstr(cn, "Short")   || strstr(cn, "Byte"))
      return 0;
    /* other classes: fall through to the optimistic answer below */
  }
  return 1;
}
static juint j_EnsureLocalCapacity(void *env, int cap) {
  (void)env;
  if (cap <= 0 || !g_local_state_key_ready)
    return 0;
  LocalState *state = local_state_get();
  return state && local_reserve(state, state->top + (size_t)cap) ? 0 : (juint)-1;
}

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env;
  if (!g_local_state_key_ready)
    return 0;  // safe leak-only fallback if the native TLS key pool is exhausted
  LocalState *state = local_state_get();
  if (!state || state->frame_top >= MAX_FRAMES)
    return (juint)-1;
  if (cap > 0 && !local_reserve(state, state->top + (size_t)cap))
    return (juint)-1;
  state->frames[state->frame_top++] = state->top;
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  LocalState *state = local_state_current();
  if (!state)
    return result;
  const size_t mark = state->frame_top > 0
                    ? state->frames[--state->frame_top] : 0;
  int result_in_parent = 0;
  for (size_t i = 0; result && i < mark; i++)
    if (state->refs[i] == result) { result_in_parent = 1; break; }
  int promoted = 0;
  for (size_t i = mark; i < state->top; i++) {
    if (state->refs[i] == result && !result_in_parent && !promoted) {
      promoted = 1;  // move exactly one owned local ref into the parent frame
      continue;
    }
    free_ref(state->refs[i]);
  }
  state->top = mark;
  if (result && !result_in_parent) {
    /* Pooled refs are deliberately not registered when created. If result was
     * not present in the popped frame, create the parent-frame ownership now. */
    if (!promoted)
      retain_ref(result);
    if (local_reserve(state, state->top + 1))
      state->refs[state->top++] = result;
  }
  return result;
}

// --- Call<type>Method (instance + static share class-aware dispatch) --------

#define CALL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; va_list va; va_start(va, id); \
    ret_t r = dispatch(recv, id, va); va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; return dispatch(recv, id, va); }

CALL_VARIADIC(j_CallObjectMethod, void *, dispatch_object)
CALL_VARIADIC(j_CallIntMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallBooleanMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallLongMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallFloatMethod, float, dispatch_float)
CALL_VARIADIC(j_CallDoubleMethod, double, dispatch_double)

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; va_list va; va_start(va, id); dispatch_void(recv, id, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; dispatch_void(recv, id, va);
}

#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticFloatMethod    j_CallFloatMethod
#define j_CallStaticFloatMethodV   j_CallFloatMethodV
#define j_CallStaticDoubleMethod   j_CallDoubleMethod
#define j_CallStaticDoubleMethodV  j_CallDoubleMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

// --- Call<type>MethodA / NewObjectA (jvalue[] args) -------------------------
// Route jvalue[] calls through the same name-based compatibility dispatch.
static void *j_CallObjectMethodA (void *e, void *r, FakeID *id, const void *a){
  int billing_handled = 0;
  void *billing_object_arg = NULL;
  int billing_int_arg = 0;
  if (a && offline_billing_method(id) &&
      !strcmp(id->name, "getBasePricingPhase"))
    billing_object_arg = ((const FakeJValue *)a)[0].l;
  else if (a && offline_billing_tag(r, TAG_BILLING_LIST) &&
           !strcmp(id->name, "get"))
    billing_int_arg = ((const FakeJValue *)a)[0].i;
  void *billing_result = offline_billing_object_common(
      r, id, billing_object_arg, billing_int_arg, &billing_handled);
  if (billing_handled)
    return billing_result;
  int zf3_handled=0;void *zf3_result=zf3_prefs_object_a(id,a,&zf3_handled);
  if(zf3_handled)return zf3_result;
  int ctr_handled=0;void *ctr_result=ctr_prefs_object_a(id,a,&ctr_handled);
  if(ctr_handled)return ctr_result;
  int reflection_handled = 0;
  void *reflection_result = reflection_object_a(id, a, &reflection_handled);
  if (reflection_handled)
    return reflection_result;
  if (a && !strcmp(id->name, "get") &&
      !strcmp(id->sig, "(Ljava/lang/Object;)Ljava/lang/Object;"))
    return zf_manager_service(((void *const *)a)[0]);
  if (a && zf_resource_owns(id) && !strcmp(id->name,"loadData"))
    return zf_resource_load(jni_string_utf(((void *const *)a)[0]));
  if (a && zf_resource_owns(id) && !strcmp(id->name,"getFiles"))
    return zf_resource_files(jni_string_utf(((void *const *)a)[0]),
                             jni_string_utf(((void *const *)a)[1]),
                             jni_string_utf(((void *const *)a)[2]));
  // getProperty()'s String key lives in jvalue[0] and does NOT survive the
  // va_list-less forward below, so pull it directly. FMOD's OpenSL output reads
  // PROPERTY_OUTPUT_FRAMES_PER_BUFFER through this "A" path; without the key it
  // got "" -> framesPerBuffer 0 -> FMOD error 60.
  if (a && name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(((void *const *)a)[0]));
  if (a && (name_has(id->cls, "net/Uri") || sig_returns(id->sig,"Ljava/lang/String;")) &&
      (!strcmp(id->name, "encode") || !strcmp(id->name, "decode"))) {
    void *arg = (void *)((void *const *)a)[0];
    if (arg && *(uint32_t *)arg == TAG_CLASS) arg = (void *)((void *const *)a)[1];
    return arg;                                  /* identity, jvalue path */
  }
  void *out=NULL;
  if (unity_dispatch_object_a(r,id,a,&out)) return out;
  return j_CallObjectMethod(e, r, id);
}
static juint j_CallBooleanMethodA(void *e, void *r, FakeID *id, const void *a){
  int zf3_handled=0;uint64_t zf3_result=zf3_prefs_int_a(id,a,&zf3_handled);if(zf3_handled)return (juint)zf3_result;
  int ctr_handled=0;uint64_t ctr_result=ctr_prefs_int_a(id,a,&ctr_handled);if(ctr_handled)return (juint)ctr_result;
  uint64_t out=0; if(unity_dispatch_int_a(r,id,a,&out))return (juint)out;
  return j_CallBooleanMethod(e,r,id); }
static juint j_CallIntMethodA    (void *e, void *r, FakeID *id, const void *a){
  int zf3_handled=0;uint64_t zf3_result=zf3_prefs_int_a(id,a,&zf3_handled);if(zf3_handled)return (juint)zf3_result;
  int ctr_handled=0;uint64_t ctr_result=ctr_prefs_int_a(id,a,&ctr_handled);if(ctr_handled)return (juint)ctr_result;
  if (zf_audio_handles(id->cls, id->name))
    return zf_audio_int_a(id->name, id->sig, a);
  if (zf_resource_owns(id) && !strcmp(id->name,"getFreeSpace")) return 0x40000000u;
  if (a && zf_resource_owns(id) && !strcmp(id->name,"isFileExists")) {
    char path[768];const char *raw=jni_string_utf(((void *const *)a)[0]);
    const int exists=zf_resource_exists(raw,path,sizeof path);
    return (juint)exists;
  }
  // parseInt/parseLong via the jvalue[] path: read the String from jvalue[0].
  if (a && (name_has(id->name, "parseInt") || name_has(id->name, "parseLong"))) {
    const char *s = jni_string_utf(((void *const *)a)[0]);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    
    return v;
  }
  uint64_t out=0; if(unity_dispatch_int_a(r,id,a,&out))return (juint)out;
  return j_CallIntMethod(e, r, id);
}
static juint j_CallLongMethodA   (void *e, void *r, FakeID *id, const void *a){
  int zf3_handled=0;uint64_t zf3_result=zf3_prefs_int_a(id,a,&zf3_handled);if(zf3_handled)return (juint)zf3_result;
  int ctr_handled=0;uint64_t ctr_result=ctr_prefs_int_a(id,a,&ctr_handled);if(ctr_handled)return (juint)ctr_result;
  uint64_t out=0; if(unity_dispatch_int_a(r,id,a,&out))return (juint)out;
  return j_CallLongMethod(e,r,id); }
static float j_CallFloatMethodA  (void *e, void *r, FakeID *id, const void *a){
  int zf3_handled=0;float zf3_result=zf3_prefs_float_a(id,a,&zf3_handled);if(zf3_handled)return zf3_result;
  int ctr_handled=0;float ctr_result=ctr_prefs_float_a(id,a,&ctr_handled);if(ctr_handled)return ctr_result;
  if (zf_audio_handles(id->cls, id->name))
    return zf_audio_float_a(id->name, id->sig, a);
  float out=0; if(unity_dispatch_float_a(r,id,a,&out))return out;
  return j_CallFloatMethod(e,r,id); }
static double j_CallDoubleMethodA(void *e,void *r,FakeID *id,const void *a){
  int handled=0;double result=zf3_prefs_double_a(id,a,&handled);if(handled)return result;
  return j_CallDoubleMethod(e,r,id);
}
static void  j_CallVoidMethodA   (void *e, void *r, FakeID *id, const void *a){
  if(offline_billing_void_a(r,id,a))return;
  if(zf3_threads_void(id))return;
  if(zf3_prefs_void_a(id,a))return;
  if(ctr_prefs_void_a(id,a))return;
  if(a&&is_video_play_method(id)){
    const FakeJValue *args=a;
    queue_video_playback(args[0].l,args[1].z,args[2].z);
    return;
  }
  if (zf_audio_handles(id->cls, id->name)) {
    zf_audio_void_a(id->name, id->sig, a);
    return;
  }
  if (a && zf_resource_owns(id)) {
    if(!strcmp(id->name,"saveData")){zf_resource_save(((void *const *)a)[0],jni_string_utf(((void *const *)a)[1]));return;}
    if(!strcmp(id->name,"createFolder")){char path[768];if(zf_resource_path(path,sizeof path,jni_string_utf(((void *const *)a)[0]))){zf_mkdir_parents(path);mkdir(path,0777);}return;}
    if(!strcmp(id->name,"remove")||!strcmp(id->name,"removeFolder")){char path[768];if(zf_resource_path(path,sizeof path,jni_string_utf(((void *const *)a)[0]))){unlink(path);rmdir(path);}return;}
  }
  if(unity_dispatch_void_a(r,id,a))return;
  j_CallVoidMethod(e,r,id); }
static void *j_NewObjectA        (void *e, void *cls, void *mid, const void *a){ (void)e;
  return new_object_dispatch(cls, mid, a ? ((void *const *)a)[0] : NULL); }
#define j_CallStaticObjectMethodA  j_CallObjectMethodA
#define j_CallStaticBooleanMethodA j_CallBooleanMethodA
#define j_CallStaticIntMethodA     j_CallIntMethodA
#define j_CallStaticLongMethodA    j_CallLongMethodA
#define j_CallStaticFloatMethodA   j_CallFloatMethodA
#define j_CallStaticDoubleMethodA  j_CallDoubleMethodA
#define j_CallStaticVoidMethodA    j_CallVoidMethodA

// --- strings ----------------------------------------------------------------

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static void *j_NewString(void *env, const uint16_t *u, int len) {
  (void)env;
  if (!u || len < 0) return jni_make_string("");
  char *tmp = malloc((size_t)len * 4 + 1);
  int o = 0;
  for (int i = 0; i < len; i++) {
    uint32_t cp = u[i];
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < len &&
        u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff) {
      cp = 0x10000u + ((cp - 0xd800u) << 10) + (u[++i] - 0xdc00u);
    } else if (cp >= 0xd800 && cp <= 0xdfff) {
      cp = 0xfffd;
    }
    if (cp < 0x80) {
      tmp[o++] = (char)cp;
    } else if (cp < 0x800) {
      tmp[o++] = (char)(0xc0 | (cp >> 6));
      tmp[o++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      tmp[o++] = (char)(0xe0 | (cp >> 12));
      tmp[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
      tmp[o++] = (char)(0x80 | (cp & 0x3f));
    } else {
      tmp[o++] = (char)(0xf0 | (cp >> 18));
      tmp[o++] = (char)(0x80 | ((cp >> 12) & 0x3f));
      tmp[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
      tmp[o++] = (char)(0x80 | (cp & 0x3f));
    }
  }
  tmp[o] = 0;
  void *s = jni_make_string(tmp);
  free(tmp);
  return s;
}
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

// JNI slots 165/166. Unity uses these while initializing Android audio and
// localization. The returned storage belongs to the caller until ReleaseStringChars.
static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  const char *utf = obj_str(jstr);
  const juint len = utf16_len(utf);
  uint16_t *chars = malloc(((size_t)len + 1) * sizeof(*chars));
  if (!chars) {
    if (is_copy) *is_copy = 0;
    return NULL;
  }
  fake_utf8_to_utf16(utf, chars);
  chars[len] = 0;
  if (is_copy) *is_copy = 1;
  return chars;
}

static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env;
  (void)jstr;
  free((void *)chars);
}

// GetStringUTFRegion: the engine reads ALL its strings through this (not
// GetStringUTFChars), so it must work. Copies the [start, start+len) region as
// modified UTF-8 into buf. Our strings are ASCII (paths / archive names), where
// UTF-16 char offsets == UTF-8 byte offsets, so a byte copy is exact.
static void j_GetStringUTFRegion(void *env, void *jstr, int start, int len, char *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)strlen(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  memcpy(buf, s + start, (size_t)len);
  buf[len] = '\0';
}
// GetStringRegion: UTF-16 offsets and output, including surrogate pairs.
static void j_GetStringRegion(void *env, void *jstr, int start, int len, uint16_t *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)utf16_len(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  uint16_t *all = malloc(((size_t)slen + 1) * sizeof(*all));
  if (!all) return;
  fake_utf8_to_utf16(s, all);
  memcpy(buf, all + start, (size_t)len * sizeof(*buf));
  free(all);
}
// GetStringLength must return the UTF-16 code-unit count, not the byte count
// (CJK text is multi-byte in UTF-8); engine code sizes UTF-16 buffers with it.
static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  return utf16_len(obj_str(jstr));
}

// --- arrays -----------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR))
    return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->refs = 1;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++) a->items[i] = init;
  return reg_local(a);
}
static void *j_GetObjectArrayElement(void *env, void *arr, int i) {
  (void)env;
  FakeObjArray *a = arr;
  return (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len) ? a->items[i] : NULL;
}
static void j_SetObjectArrayElement(void *env, void *arr, int i, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len) a->items[i] = val;
}

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

// --- fields -----------------------------------------------------------------
// The engine and game read Java fields (android.os.Build.*, Build.VERSION.SDK_INT,
// PackageInfo.versionName/versionCode, DisplayMetrics.*). Route every read through a
// name-based dispatcher on the FakeID; a universal null/0 blanked the app version and
// zeroed display metrics.
#define APP_VERSION_NAME GAME_VERSION_NAME
#define APP_VERSION_CODE GAME_VERSION_CODE
#define NX_SDK_INT       33        /* Android 13 -- high enough to pass any minSdk gate  */

static void *field_object(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  // PackageInfo / ApplicationInfo version string
  if (!strcmp(n, "versionName")) return jni_make_string(APP_VERSION_NAME);
  // UnityPlayer.currentActivity is THE Activity -- null NPEs every currentActivity
  // .getXxx() in managed code, so hand back a live opaque Activity.
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "currentActivity")) return jni_make_object("android/app/Activity");
    // Some plugins use UnityPlayer.activityContext instead of currentActivity.
    if (!strcmp(n, "activityContext"))  return jni_make_object("android/app/Activity");
    if (!strcmp(n, "MANUFACTURER"))    return jni_make_string("Nintendo");
    // Keep the Android permissions plugin handle non-null.
    if (!strcmp(n, "androidPermissionsPlugin")) {
      void *plugin = jni_make_object("com/rovio/permissions/AndroidPermissionsPlugin");
      
      return plugin;
    }
  }
  // AudioManager.PROPERTY_OUTPUT_* static keys, read just before getProperty(key).
  // Record which one in g_last_output_prop so getProperty() can answer when the key
  // argument is lost on the JNI call path (see getproperty_value).
  if (name_has(c, "media/AudioManager")) {
    if (!strcmp(n, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER")) { g_last_output_prop = 2; return jni_make_string("android.media.property.OUTPUT_FRAMES_PER_BUFFER"); }
    if (!strcmp(n, "PROPERTY_OUTPUT_SAMPLE_RATE"))       { g_last_output_prop = 1; return jni_make_string("android.media.property.OUTPUT_SAMPLE_RATE"); }
  }
  // Context.*_SERVICE name constants -> the strings getSystemService() expects
  if (name_has(c, "content/Context")) {
    if (!strcmp(n, "AUDIO_SERVICE"))        return jni_make_string("audio");
    if (!strcmp(n, "DISPLAY_SERVICE"))      return jni_make_string("display");
    if (!strcmp(n, "WINDOW_SERVICE"))       return jni_make_string("window");
    if (!strcmp(n, "LOCATION_SERVICE"))     return jni_make_string("location");
    if (!strcmp(n, "CONNECTIVITY_SERVICE")) return jni_make_string("connectivity");
    if (!strcmp(n, "MEDIA_ROUTER_SERVICE")) return jni_make_string("media_router");
    if (!strcmp(n, "VIBRATOR_SERVICE"))     return jni_make_string("vibrator");
  }
  // Environment.MEDIA_MOUNTED MUST equal getExternalStorageState()'s return
  // ("mounted", set in act_object) or the storage check fails and save data is
  // disabled. Keep both in lockstep.
  if (name_has(c, "os/Environment")) {
    if (!strcmp(n, "MEDIA_MOUNTED"))           return jni_make_string("mounted");
    if (!strcmp(n, "MEDIA_MOUNTED_READ_ONLY")) return jni_make_string("mounted_ro");
  }
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "FEATURE_AUDIO_LOW_LATENCY")) return jni_make_string("android.hardware.audio.low_latency");
    if (!strcmp(n, "FEATURE_AUDIO_PRO"))         return jni_make_string("android.hardware.audio.pro");
  }
  // android.os.Build identity strings (all public static final String)
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "MODEL"))        return jni_make_string("Switch");
    if (!strcmp(n, "MANUFACTURER")) return jni_make_string("Nintendo");
    if (!strcmp(n, "BRAND"))        return jni_make_string("Nintendo");
    if (!strcmp(n, "DEVICE"))       return jni_make_string("Switch");
    if (!strcmp(n, "PRODUCT"))      return jni_make_string("Switch");
    if (!strcmp(n, "HARDWARE"))     return jni_make_string("nx");
    if (!strcmp(n, "BOARD"))        return jni_make_string("nx");
    if (!strcmp(n, "DISPLAY"))      return jni_make_string("nx");
    if (!strcmp(n, "ID"))           return jni_make_string("REL");
    if (!strcmp(n, "TYPE"))         return jni_make_string("user");
    if (!strcmp(n, "TAGS"))         return jni_make_string("release-keys");
    if (!strcmp(n, "FINGERPRINT"))  return jni_make_string("Nintendo/Switch/Switch:13/REL/10007:user/release-keys");
    if (!strcmp(n, "BOOTLOADER"))   return jni_make_string("unknown");
    if (!strcmp(n, "HOST"))         return jni_make_string("localhost");
    if (!strcmp(n, "USER"))         return jni_make_string("nx");
    if (!strcmp(n, "SERIAL"))       return jni_make_string("unknown");
    if (!strcmp(n, "RELEASE"))      return jni_make_string("13");        /* Build.VERSION.* */
    if (!strcmp(n, "CODENAME"))     return jni_make_string("REL");
    if (!strcmp(n, "INCREMENTAL"))  return jni_make_string("10007");
    if (!strcmp(n, "SECURITY_PATCH")) return jni_make_string("2023-01-01");
    if (!strcmp(n, "BASE_OS"))      return jni_make_string("");
  }
  // Any other String-typed field -> "" (non-null avoids NPEs in string ops).
  if (sig_returns(id->sig, "Ljava/lang/String;")) return jni_make_string("");
  // Any other object field stays null; array fields handled by the caller.
  return NULL;
}

static juint field_int(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  if (!strcmp(n, "versionCode")) return APP_VERSION_CODE;
  // UnityPlayer integer statics
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "SDK_INT"))     return NX_SDK_INT;
    if (!strcmp(n, "densityDpi"))  return 320;
    if (!strcmp(n, "widthPixels")) return 720;    /* fbstub45 PORTRAIT (stable) */
    if (!strcmp(n, "heightPixels"))return 1280;
    if (!strcmp(n, "STREAM_MUSIC"))return 3;   /* AudioManager.STREAM_MUSIC      */
    if (!strcmp(n, "GET_DEVICES_OUTPUTS")) return 2; /* AudioManager.GET_DEVICES_OUTPUTS */
    if (!strcmp(n, "ROUTE_TYPE_LIVE_VIDEO")) return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_UNSPECIFIED"))       return -1;
    if (!strcmp(n, "SCREEN_ORIENTATION_LANDSCAPE"))         return 0;
    if (!strcmp(n, "SCREEN_ORIENTATION_PORTRAIT"))          return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_LANDSCAPE")) return 8;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_PORTRAIT"))  return 9;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_USER"))         return 13;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_SENSOR"))       return 10;
  }
  if (name_has(c, "content/Context") && !strcmp(n, "MODE_PRIVATE")) return 0;
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "PERMISSION_GRANTED")) return 0;   /* == granted              */
    if (!strcmp(n, "PERMISSION_DENIED"))  return (juint)-1;
  }
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "SDK_INT"))          return NX_SDK_INT;
    if (!strcmp(n, "PREVIEW_SDK_INT"))  return 0;
  }
  // DisplayMetrics integer fields (width/height/dpi)
  if (name_has(c, "DisplayMetrics")) {
    if (!strcmp(n, "widthPixels"))  return 720;    /* fbstub45 PORTRAIT (stable) */
    if (!strcmp(n, "heightPixels")) return 1280;
    if (!strcmp(n, "densityDpi"))   return 320;    /* xhdpi bucket                */
  }
  return 0;
}

/* DisplayMetrics.density / xdpi / ydpi / scaledDensity are float fields. 0 would
 * make dp->px scaling collapse, so hand back a sane xhdpi density (2.0). */
static float field_float(const FakeID *id) {
  const char *n = id->name;
  if (name_has(id->cls, "DisplayMetrics")) {
    if (!strcmp(n, "density") || !strcmp(n, "scaledDensity")) return 2.0f;
    if (!strcmp(n, "xdpi") || !strcmp(n, "ydpi"))             return 320.0f;
  }
  return 0.0f;
}

static void *j_GetObjectField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return NULL;
  return field_object((const FakeID *)fid); }
static juint j_GetIntField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0;
  return field_int((const FakeID *)fid); }
static juint j_GetLongField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return (juint)field_int((const FakeID *)fid); }
static juint j_GetBooleanField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return field_int((const FakeID *)fid) ? 1 : 0; }
static float j_GetFloatField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0.0f; return field_float((const FakeID *)fid); }
static double j_GetDoubleField(void *env,void *obj,void *fid){
  (void)env;(void)obj;if(!fid)return 0.0;return (double)field_float((const FakeID *)fid);}

// --- reflection bridge (proxy support) --------------------------------------
// Unity's AndroidJavaProxy converts reflected Method/Field objects into
// jmethod/jfieldIDs via these. We carry no real reflection, but a non-null opaque
// ID lets the proxy bind and store; an invoked callback routes through act_* and
// no-ops, which is right for our stubbed events.
static void *j_FromReflectedMethod(void *env, void *m) {
  (void)env;
  if (m && *(uint32_t *)m == TAG_ID) return m;
  return get_id("java/lang/reflect/Method", "invoke", "()V"); }
static void *j_FromReflectedField(void *env, void *f) {
  (void)env;
  if (f && *(uint32_t *)f == TAG_ID) return f;
  return get_id("java/lang/reflect/Field", "field", "()V"); }
static void *j_ToReflectedMethod(void *env, void *cls, void *mid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return mid ? mid : jni_make_object("java/lang/reflect/Method"); }
static void *j_ToReflectedField(void *env, void *cls, void *fid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return fid ? fid : jni_make_object("java/lang/reflect/Field"); }

// --- misc -------------------------------------------------------------------

typedef struct { const char *name; const char *sig; void *fn; } JNINativeMethod_;
static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env;
  const char *cn = class_name_of(cls);
  const JNINativeMethod_ *m = methods;
  if(m&&name_has(cn,"com/zf3/threads/AndroidThreadManager")){
    for(int i=0;i<n;i++)if(m[i].name&&m[i].fn&&
       !strcmp(m[i].name,"fetchAndRunQueuedUIOperations")){
      g_zf3_threads.fetch=(Zf3UiFetchFn)m[i].fn;
      break;
    }
  }
  if(m&&name_has(cn,"com/zf3/billing/google/GoogleIapManager")){
    mutexLock(&g_offline_billing.lock);
    for(int i=0;i<n;i++){
      if(!m[i].name||!m[i].fn)continue;
      if(!strcmp(m[i].name,"onSetupFinished"))
        g_offline_billing.on_setup_finished=(BillingBoolFn)m[i].fn;
      else if(!strcmp(m[i].name,"onStoreLocaleDetected"))
        g_offline_billing.on_store_locale_detected=(BillingStringFn)m[i].fn;
      else if(!strcmp(m[i].name,"onProductDetailsReceived"))
        g_offline_billing.on_product_details_received=(BillingObjectFn)m[i].fn;
      else if(!strcmp(m[i].name,"onRequestProductsSucceeded"))
        g_offline_billing.on_request_products_succeeded=(BillingVoidFn)m[i].fn;
      else if(!strcmp(m[i].name,"onPurchaseSucceeded"))
        g_offline_billing.on_purchase_succeeded=(BillingPurchaseFn)m[i].fn;
      else if(!strcmp(m[i].name,"onRestorePurchasesSucceeded"))
        g_offline_billing.on_restore_purchases_succeeded=(BillingVoidFn)m[i].fn;
      else if(!strcmp(m[i].name,"onConsumeSucceeded"))
        g_offline_billing.on_consume_succeeded=(BillingStringFn)m[i].fn;
      else if(!strcmp(m[i].name,"onAcknowledgeSucceeded"))
        g_offline_billing.on_acknowledge_succeeded=(BillingStringFn)m[i].fn;
      else continue;
    }
    mutexUnlock(&g_offline_billing.lock);
  }
  if(m&&name_has(cn,"com/zf3/preferences/AndroidPreferences")){
    g_zf3_pref_callbacks.clazz=cls;
    for(int i=0;i<n;i++){
      if(!m[i].name||!m[i].fn)continue;
      if(!strcmp(m[i].name,"onBool"))g_zf3_pref_callbacks.on_bool=(Zf3PrefBoolFn)m[i].fn;
      else if(!strcmp(m[i].name,"onDouble"))g_zf3_pref_callbacks.on_double=(Zf3PrefDoubleFn)m[i].fn;
      else if(!strcmp(m[i].name,"onFloat"))g_zf3_pref_callbacks.on_float=(Zf3PrefFloatFn)m[i].fn;
      else if(!strcmp(m[i].name,"onInt"))g_zf3_pref_callbacks.on_int=(Zf3PrefIntFn)m[i].fn;
      else if(!strcmp(m[i].name,"onLong"))g_zf3_pref_callbacks.on_long=(Zf3PrefLongFn)m[i].fn;
      else if(!strcmp(m[i].name,"onString"))g_zf3_pref_callbacks.on_string=(Zf3PrefStringFn)m[i].fn;
      else continue;
    }
  }
  if (m && name_has(cn, "unity3d/player/UnityPlayer")) {
    for (int i = 0; i < n; ++i) {
      if (!m[i].name) continue;
      if (!strcmp(m[i].name, "nativeSetKeyboardIsVisible"))
        g_unity_keyboard_visible = (UnityKeyboardVisibleFn)m[i].fn;
      else if (!strcmp(m[i].name, "nativeSetInputString"))
        g_unity_input_string = (UnityInputStringFn)m[i].fn;
      else if (!strcmp(m[i].name, "nativeSoftInputClosed"))
        g_unity_soft_input_closed = (UnitySoftInputFn)m[i].fn;
      else if (!strcmp(m[i].name, "nativeSoftInputCanceled"))
        g_unity_soft_input_canceled = (UnitySoftInputFn)m[i].fn;
    }
  }
  return 0;
}

void jni_process_zf3_ui_callbacks(void){
  /* A fetch drains the queue; cap immediate self-requeues per frame. */
  for(int pass=0;pass<8;pass++){
    if(!__atomic_exchange_n(&g_zf3_threads.pending,0,__ATOMIC_ACQ_REL))return;
    if(!g_zf3_threads.fetch||!g_zf3_threads.native_handle)return;
    g_zf3_threads.fetch(fake_env,g_zf3_threads.object,g_zf3_threads.native_handle);
  }
}

void jni_process_offline_billing(void) {
  enum {
    BILLING_EVENT_NONE,
    BILLING_EVENT_SETUP,
    BILLING_EVENT_LOCALE,
    BILLING_EVENT_PRODUCTS,
    BILLING_EVENT_PURCHASE,
    BILLING_EVENT_RESTORE,
    BILLING_EVENT_CONSUME,
    BILLING_EVENT_ACKNOWLEDGE
  } event = BILLING_EVENT_NONE;

  void *object = NULL;
  BillingBoolFn on_setup_finished = NULL;
  BillingStringFn on_store_locale_detected = NULL;
  BillingObjectFn on_product_details_received = NULL;
  BillingVoidFn on_request_products_succeeded = NULL;
  BillingPurchaseFn on_purchase_succeeded = NULL;
  BillingVoidFn on_restore_purchases_succeeded = NULL;
  BillingStringFn on_consume_succeeded = NULL;
  BillingStringFn on_acknowledge_succeeded = NULL;
  unsigned catalogue_count = 0;
  unsigned purchase_serial = 0;
  char catalogue[OFFLINE_BILLING_MAX_PRODUCTS][OFFLINE_BILLING_ID_LENGTH];
  char value[OFFLINE_BILLING_ID_LENGTH] = "";

  mutexLock(&g_offline_billing.lock);
  object = g_offline_billing.object;
  on_setup_finished = g_offline_billing.on_setup_finished;
  on_store_locale_detected = g_offline_billing.on_store_locale_detected;
  on_product_details_received = g_offline_billing.on_product_details_received;
  on_request_products_succeeded = g_offline_billing.on_request_products_succeeded;
  on_purchase_succeeded = g_offline_billing.on_purchase_succeeded;
  on_restore_purchases_succeeded = g_offline_billing.on_restore_purchases_succeeded;
  on_consume_succeeded = g_offline_billing.on_consume_succeeded;
  on_acknowledge_succeeded = g_offline_billing.on_acknowledge_succeeded;

  if (g_offline_billing.pending_setup) {
    g_offline_billing.pending_setup = 0;
    event = BILLING_EVENT_SETUP;
  } else if (g_offline_billing.pending_locale) {
    g_offline_billing.pending_locale = 0;
    event = BILLING_EVENT_LOCALE;
  } else if (g_offline_billing.pending_products) {
    g_offline_billing.pending_products = 0;
    event = BILLING_EVENT_PRODUCTS;
    catalogue_count = g_offline_billing.catalogue_count;
    memcpy(catalogue, g_offline_billing.catalogue,
           catalogue_count * OFFLINE_BILLING_ID_LENGTH);
  } else if (g_offline_billing.pending_purchase) {
    g_offline_billing.pending_purchase = 0;
    event = BILLING_EVENT_PURCHASE;
    purchase_serial = ++g_offline_billing.purchase_serial;
    snprintf(value, sizeof(value), "%s", g_offline_billing.purchase_id);
  } else if (g_offline_billing.pending_consume) {
    g_offline_billing.pending_consume = 0;
    event = BILLING_EVENT_CONSUME;
    snprintf(value, sizeof(value), "%s", g_offline_billing.consume_id);
  } else if (g_offline_billing.pending_acknowledge) {
    g_offline_billing.pending_acknowledge = 0;
    event = BILLING_EVENT_ACKNOWLEDGE;
    snprintf(value, sizeof(value), "%s", g_offline_billing.acknowledge_id);
  } else if (g_offline_billing.pending_restore) {
    g_offline_billing.pending_restore = 0;
    event = BILLING_EVENT_RESTORE;
  }
  mutexUnlock(&g_offline_billing.lock);

  if (!object)
    return;
  switch (event) {
    case BILLING_EVENT_SETUP:
      if (on_setup_finished)
        on_setup_finished(fake_env, object, 1);
      break;
    case BILLING_EVENT_LOCALE:
      if (on_store_locale_detected)
        on_store_locale_detected(fake_env, object, jni_make_string("US"));
      break;
    case BILLING_EVENT_PRODUCTS:
      if (on_product_details_received)
        for (unsigned i = 0; i < catalogue_count; ++i) {
          FakeBillingItem *product = offline_billing_product(catalogue[i]);
          if (product)
            on_product_details_received(fake_env, object, product);
        }
      if (on_request_products_succeeded)
        on_request_products_succeeded(fake_env, object);
      break;
    case BILLING_EVENT_PURCHASE:
      if (on_purchase_succeeded) {
        char order_id[64];
        char token[OFFLINE_BILLING_ID_LENGTH + 32];
        snprintf(order_id, sizeof(order_id), "nx-offline-%08u", purchase_serial);
        snprintf(token, sizeof(token), "nx:%s:%u", value, purchase_serial);
        on_purchase_succeeded(fake_env, object, jni_make_string(value),
                              jni_make_string(order_id), jni_make_string(token));
      }
      break;
    case BILLING_EVENT_RESTORE:
      if (on_restore_purchases_succeeded)
        on_restore_purchases_succeeded(fake_env, object);
      break;
    case BILLING_EVENT_CONSUME:
      if (on_consume_succeeded)
        on_consume_succeeded(fake_env, object, jni_make_string(value));
      break;
    case BILLING_EVENT_ACKNOWLEDGE:
      if (on_acknowledge_succeeded)
        on_acknowledge_succeeded(fake_env, object, jni_make_string(value));
      break;
    default:
      break;
  }
}

void jni_process_soft_keyboard(void) {
  struct {
    int keyboard_type;
    int secure;
    int character_limit;
    char initial[0x1001];
    char guide[0x101];
  } request;

  mutexLock(&g_soft_keyboard.lock);
  if (!g_soft_keyboard.pending) {
    mutexUnlock(&g_soft_keyboard.lock);
    return;
  }
  request.keyboard_type = g_soft_keyboard.keyboard_type;
  request.secure = g_soft_keyboard.secure;
  request.character_limit = g_soft_keyboard.character_limit;
  snprintf(request.initial, sizeof request.initial, "%s",
           g_soft_keyboard.initial);
  snprintf(request.guide, sizeof request.guide, "%s",
           g_soft_keyboard.guide);
  g_soft_keyboard.pending = 0;
  mutexUnlock(&g_soft_keyboard.lock);

  extern void *fake_unityplayer_thiz;
  if (g_unity_keyboard_visible)
    g_unity_keyboard_visible(fake_env, fake_unityplayer_thiz, 1);

  char result[0x1001];
  snprintf(result, sizeof result, "%s", request.initial);
  SwkbdConfig keyboard;
  Result rc = swkbdCreate(&keyboard, 0);
  if (R_SUCCEEDED(rc)) {
    if (request.secure)
      swkbdConfigMakePresetPassword(&keyboard);
    else
      swkbdConfigMakePresetDefault(&keyboard);
    if (request.keyboard_type == 4 || request.keyboard_type == 5 ||
        request.keyboard_type == 11)
      swkbdConfigSetType(&keyboard, SwkbdType_NumPad);
    swkbdConfigSetStringLenMax(&keyboard,
                               (uint32_t)request.character_limit);
    swkbdConfigSetInitialText(&keyboard, request.initial);
    if (request.guide[0])
      swkbdConfigSetGuideText(&keyboard, request.guide);
    rc = swkbdShow(&keyboard, result, sizeof result);
    swkbdClose(&keyboard);
  }

  if (R_SUCCEEDED(rc)) {
    if (g_unity_input_string)
      g_unity_input_string(fake_env, fake_unityplayer_thiz,
                           jni_make_string(result));
    if (g_unity_soft_input_closed)
      g_unity_soft_input_closed(fake_env, fake_unityplayer_thiz);
  } else {
    if (g_unity_soft_input_canceled)
      g_unity_soft_input_canceled(fake_env, fake_unityplayer_thiz);
  }
  if (g_unity_keyboard_visible)
    g_unity_keyboard_visible(fake_env, fake_unityplayer_thiz, 0);
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void1(void *env) { (void)env; }

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
/* Accessors used by unity_jni.c and unity_input.c to read
 * (otherwise static) FakeString / FakePriArray without duplicating the structs. */
void *jni_bytearray_data(void *arr, int *len_out) {
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR) { if (len_out) *len_out = a->len; return a->data; }
  if (len_out) *len_out = 0;
  return NULL;
}
const char *jni_string_utf(void *jstr) {
  FakeString *s = jstr;
  return (s && s->tag == TAG_STRING) ? s->utf : "";
}

void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  unity_jni_init(GAME_HOME);
  mutexInit(&pool_lock);
  mutexInit(&g_soft_keyboard.lock);
  mutexInit(&g_offline_billing.lock);
  if (pthread_key_create(&g_local_state_key, local_state_destroy) == 0) {
    g_local_state_key_ready = 1;
  }

  jni_fill_unimpl(env_table); // indexed stubs: log the exact unimplemented slot

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[7]   = (void *)j_FromReflectedMethod;    // was UNIMPL (proxy bind)
  env_table[8]   = (void *)j_FromReflectedField;
  env_table[9]   = (void *)j_ToReflectedMethod;
  env_table[12]  = (void *)j_ToReflectedField;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_IsInstanceOf;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[58]  = (void *)j_CallDoubleMethod;
  env_table[59]  = (void *)j_CallDoubleMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  // "A" (jvalue[]) variants -- instance
  env_table[30]  = (void *)j_NewObjectA;
  env_table[36]  = (void *)j_CallObjectMethodA;
  env_table[39]  = (void *)j_CallBooleanMethodA;
  env_table[51]  = (void *)j_CallIntMethodA;
  env_table[54]  = (void *)j_CallLongMethodA;
  env_table[57]  = (void *)j_CallFloatMethodA;
  env_table[60]  = (void *)j_CallDoubleMethodA;
  env_table[63]  = (void *)j_CallVoidMethodA;
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[96]  = (void *)j_GetBooleanField;        // GetBooleanField
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;           // GetLongField
  env_table[102] = (void *)j_GetFloatField;          // GetFloatField
  env_table[103] = (void *)j_GetDoubleField;         // GetDoubleField
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[138] = (void *)j_CallStaticDoubleMethod;
  env_table[139] = (void *)j_CallStaticDoubleMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  // "A" (jvalue[]) variants -- static (SWIG / AndroidJavaObject.CallStatic<T>)
  env_table[116] = (void *)j_CallStaticObjectMethodA;
  env_table[119] = (void *)j_CallStaticBooleanMethodA;
  env_table[131] = (void *)j_CallStaticIntMethodA;
  env_table[134] = (void *)j_CallStaticLongMethodA;
  env_table[137] = (void *)j_CallStaticFloatMethodA;
  env_table[140] = (void *)j_CallStaticDoubleMethodA;
  env_table[143] = (void *)j_CallStaticVoidMethodA;
  env_table[144] = (void *)j_GetFieldID;             // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField;         // GetStaticObjectField
  env_table[146] = (void *)j_GetBooleanField;        // GetStaticBooleanField
  env_table[150] = (void *)j_GetIntField;            // GetStaticIntField
  env_table[151] = (void *)j_GetLongField;           // GetStaticLongField
  env_table[152] = (void *)j_GetFloatField;          // GetStaticFloatField
  env_table[153] = (void *)j_GetDoubleField;         // GetStaticDoubleField
  env_table[163] = (void *)j_NewString;
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[220] = (void *)j_GetStringRegion;
  env_table[221] = (void *)j_GetStringUTFRegion; // engine reads every string via this
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}
