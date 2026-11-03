/* Shared between the Intel High Definition Audio files: hda_ctrl.c (the controller) and hda_codec.c (codec graph).
 * Numbers are from the Intel "High Definition Audio Specification" rev 1.0a (sections 3.3 controller registers,
 * 7.3 codec verbs and parameters). See include/hda.h for the overview. */
#ifndef HARIS_HDA_INTERNAL_H
#define HARIS_HDA_INTERNAL_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

/* ---- codec verbs (spec 7.3.3). A command is: codec address 31:28, node id 27:20, verb 19:0. ---- */
#define HDA_VERB_GET_PARAM 0xF00
#define HDA_VERB_GET_CONN_SEL 0xF01
#define HDA_VERB_SET_CONN_SEL 0x701
#define HDA_VERB_GET_CONN_LIST 0xF02
#define HDA_VERB_SET_POWER 0x705
#define HDA_VERB_SET_STREAM 0x706 /* payload: stream tag 7:4, lowest channel 3:0 */
#define HDA_VERB_SET_PIN_CTL 0x707
#define HDA_VERB_SET_EAPD 0x70C
#define HDA_VERB_GET_CONFIG 0xF1C
#define HDA_VERB4_SET_FORMAT 0x2 /* 4-bit verbs carry a 16-bit payload */
#define HDA_VERB4_SET_AMP 0x3

/* Get Parameter ids (spec 7.3.4) */
#define HDA_PAR_VENDOR 0x00
#define HDA_PAR_NODES 0x04     /* first sub-node 23:16, count 7:0 */
#define HDA_PAR_FG_TYPE 0x05   /* 7:0, 0x01 = audio function group */
#define HDA_PAR_WCAPS 0x09     /* audio widget capabilities */
#define HDA_PAR_PINCAPS 0x0C
#define HDA_PAR_IN_AMP 0x0D    /* same layout as HDA_PAR_OUT_AMP */
#define HDA_PAR_CONN_LEN 0x0E  /* length 6:0, long form bit 7 */
#define HDA_PAR_OUT_AMP 0x12   /* offset 6:0 (the 0 dB step), steps 14:8, step size 22:16, mute capable 31 */

/* widget capabilities (parameter 0x09) */
#define HDA_WCAP_TYPE(c) (((c) >> 20) & 0xFu)
#define HDA_WCAP_IN_AMP (1u << 1)
#define HDA_WCAP_OUT_AMP (1u << 2)
#define HDA_WCAP_AMP_OVRD (1u << 3) /* the widget has its own amp parameters (else the function group's apply) */
#define HDA_WCAP_CONN_LIST (1u << 8)
#define HDA_WCAP_POWER (1u << 10)
#define HDA_WCAP_DIGITAL (1u << 9)
#define HDA_W_OUTPUT 0x0 /* DAC */
#define HDA_W_INPUT 0x1  /* ADC */
#define HDA_W_MIXER 0x2
#define HDA_W_SELECTOR 0x3
#define HDA_W_PIN 0x4

/* pin capabilities (parameter 0x0C) */
#define HDA_PINCAP_HP_DRIVE (1u << 3)
#define HDA_PINCAP_OUT (1u << 4)
#define HDA_PINCAP_EAPD (1u << 16)

/* configuration default (verb F1C): what the board maker says the pin is wired to */
#define HDA_CFG_CONN(c) (((c) >> 30) & 3u) /* 0 jack, 1 nothing connected, 2 fixed (built in), 3 both */
#define HDA_CFG_DEVICE(c) (((c) >> 20) & 0xFu)
#define HDA_DEV_LINE_OUT 0x0
#define HDA_DEV_SPEAKER 0x1
#define HDA_DEV_HP_OUT 0x2

/* pin widget control (verb 707) */
#define HDA_PINCTL_OUT_EN (1u << 6)
#define HDA_PINCTL_HP_EN (1u << 7)

/* ---- the codec graph ---- */
#define HDA_MAX_NODES 96 /* widgets of one function group (Realtek laptop codecs have about 40) */
#define HDA_MAX_CONN 16  /* inputs remembered per widget */
#define HDA_MAX_PATH 6   /* pin, up to 4 mixers / selectors, DAC */
#define HDA_MAX_OUTS 3   /* speaker, headphone, line out: all play the same stream */

typedef struct
{
    u32 caps;    /* parameter 0x09 */
    u32 pincaps; /* pins only */
    u32 config;  /* pins only: configuration default */
    u32 in_amp;  /* input amp caps that apply (its own or the function group's) */
    u32 out_amp; /* output amp caps that apply */
    u8 nconn;
    u8 conn[HDA_MAX_CONN];
} HdaNode;

typedef struct
{
    u8 len;               /* nodes in nid[]: nid[0] = the pin, nid[len - 1] = the DAC */
    u8 nid[HDA_MAX_PATH];
    u8 sel[HDA_MAX_PATH]; /* sel[i] = index in nid[i]'s connection list that leads to nid[i + 1] */
    u8 device;            /* HDA_DEV_* of the pin */
} HdaPath;

/* Sends one verb, returns the 32-bit response or HDA_NO_RESPONSE. cad = codec address 0..14. */
#define HDA_NO_RESPONSE 0xFFFFFFFFu
typedef u32 (*HdaVerbFn)(void *ctx, unsigned cad, unsigned nid, u32 verb);

typedef struct
{
    unsigned cad;
    u32 vendor; /* vendor 31:16, device 15:0 (e.g. 0x10EC0236 = Realtek ALC236) */
    unsigned afg;   /* node id of the audio function group, 0 = none */
    unsigned first; /* node id of node[0] */
    unsigned count;
    HdaNode node[HDA_MAX_NODES];
    int nouts;
    HdaPath out[HDA_MAX_OUTS];
} HdaCodec;

/* Reads the codec's audio function group and every widget in it. 0 = done, <0 = HDA_ERR_*. */
int hda_codec_scan(HdaCodec *c, unsigned cad, HdaVerbFn fn, void *ctx);
/* Finds pin -> ... -> DAC paths for output pins (speaker first, then headphone, then line out). Returns nouts. */
int hda_codec_find_outputs(HdaCodec *c);
/* Powers up, routes and unmutes every found path and points its DAC at stream `tag` with the stream format `fmt`. */
void hda_codec_setup_outputs(const HdaCodec *c, unsigned tag, u16 fmt, HdaVerbFn fn, void *ctx);
/* Stops the DACs (stream tag 0). */
void hda_codec_stop_outputs(const HdaCodec *c, HdaVerbFn fn, void *ctx);

/* the node record for a node id, or NULL */
const HdaNode *hda_codec_node(const HdaCodec *c, unsigned nid);

#define HDA_ERR_NO_AFG -1  /* no audio function group (a modem codec, or nothing answered) */
#define HDA_ERR_SILENT -2  /* the codec stopped answering in the middle */
#define HDA_ERR_TOO_BIG -3 /* more widgets than HDA_MAX_NODES */

#endif
