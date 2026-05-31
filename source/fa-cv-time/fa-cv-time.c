#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/forge.h>
#include <lv2/lv2plug.in/ns/ext/log/logger.h>
#include <lv2/lv2plug.in/ns/ext/midi/midi.h>
#include "lv2/lv2plug.in/ns/ext/time/time.h"
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>

#ifndef DEBUG
#define DEBUG 0
#endif
#define debug_print(...) \
((void)((DEBUG) ? fprintf(stderr, __VA_ARGS__) : 0))


#ifndef M_PI
#    define M_PI 3.14159265
#endif
#define PLUGIN_URI "http://moddevices.com/plugins/mod-devel/fa-cv-time"

typedef enum {
    CV_OUTPUT      = 0,
    BPM_PORT       = 1,
    DIVISIONS_PORT = 2,
    MIN_TIME_PORT  = 3,
    MAX_TIME_PORT  = 4,
    SYNC_PORT      = 5,
    CONTROL_PORT   = 6
} PortIndex;

typedef struct {
    LV2_URID atom_Blank;
    LV2_URID atom_Float;
    LV2_URID atom_Object;
    LV2_URID atom_Path;
    LV2_URID atom_Resource;
    LV2_URID atom_Sequence;
    LV2_URID time_Position;
    LV2_URID time_barBeat;
    LV2_URID time_beatsPerMinute;
    LV2_URID time_speed;
} TimeURIs;

typedef struct {
    LV2_URID_Map*  map; // URID map feature
    LV2_Log_Log*   log;
    LV2_Log_Logger logger; // Logger API
    TimeURIs       uris; // Cache of mapped URIDs

    float* cvOutput;
    float* changeBpm;
    float  divisions;
    float* changedDiv;
    float* minTime; // Minimum time in milliseconds (maps to 0V)
    float* maxTime; // Maximum time in milliseconds (maps to 10V)
    double samplerate;
    LV2_Atom_Sequence* control;
    int* sync;
    int  prevSync;
    // Variables to keep track of the tempo information sent by the host
    float    bpm; // Beats per minute (tempo)
    uint32_t pos;
    uint32_t period; // Period in samples
    
    float speed; // Transport speed (usually 0=stop, 1=play)
    float prevSpeed;
    float beatInMeasure;
    
    float current_time_ms; // Current time in milliseconds
} CvTime;


static void
connect_port(LV2_Handle instance,
             uint32_t   port,
             void*      data)
{
    CvTime* self = (CvTime*)instance;

    switch ((PortIndex)port) {
        case CV_OUTPUT:
            self->cvOutput = (float*)data;
            break;
        case BPM_PORT:
            self->changeBpm = (float*)data;
            break;
        case DIVISIONS_PORT:
            self->changedDiv = (float*)data;
            break;
        case MIN_TIME_PORT:
            self->minTime = (float*)data;
            break;
        case MAX_TIME_PORT:
            self->maxTime = (float*)data;
            break;
        case SYNC_PORT:
            self->sync = (int*)data;
            break;
        case CONTROL_PORT:
            self->control = (LV2_Atom_Sequence*)data;
            break;
    }
}

static void
activate(LV2_Handle instance)
{
    CvTime* self = (CvTime*)instance;

    self->bpm = *self->changeBpm;
    self->divisions = *self->changedDiv;
    self->pos = 0;
}

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                    rate,
            const char*               bundle_path,
            const LV2_Feature* const* features)
{
    CvTime* self = (CvTime*)calloc(1, sizeof(CvTime));
    if (!self)
    {
        return NULL;
    }

    for (uint32_t i=0; features[i]; ++i)
    {
        if (!strcmp (features[i]->URI, LV2_URID__map))
        {
            self->map = (LV2_URID_Map*)features[i]->data;
        }
        else if (!strcmp (features[i]->URI, LV2_LOG__log))
        {
            self->log = (LV2_Log_Log*)features[i]->data;
        }
    }

    lv2_log_logger_init (&self->logger, self->map, self->log);

    if (!self->map) {
        lv2_log_error (&self->logger, "fa-cv-time.lv2 error: Host does not support urid:map\n");
        free (self);
        return NULL;
    }

    // Map URIS
    TimeURIs* const    uris  = &self->uris;
    LV2_URID_Map* const map   = self->map;
    uris->atom_Blank          = map->map(map->handle, LV2_ATOM__Blank);
    uris->atom_Float          = map->map(map->handle, LV2_ATOM__Float);
    uris->atom_Object         = map->map(map->handle, LV2_ATOM__Object);
    uris->atom_Path           = map->map(map->handle, LV2_ATOM__Path);
    uris->atom_Resource       = map->map(map->handle, LV2_ATOM__Resource);
    uris->atom_Sequence       = map->map(map->handle, LV2_ATOM__Sequence);
    uris->time_Position       = map->map(map->handle, LV2_TIME__Position);
    uris->time_barBeat        = map->map(map->handle, LV2_TIME__barBeat);
    uris->time_beatsPerMinute = map->map(map->handle, LV2_TIME__beatsPerMinute);
    uris->time_speed          = map->map(map->handle, LV2_TIME__speed);

    self->samplerate = rate;
    self->prevSync   = 0;
    self->beatInMeasure = 0;
    self->prevSpeed = 0;
    self->current_time_ms = 0.0f;

    return (LV2_Handle)self;
}


// Update the current position based on a host message.  This is called by
// run() when a time:Position is received.

static void
update_position(CvTime* self, const LV2_Atom_Object* obj)
{
    const TimeURIs* uris = &self->uris;

    // Received new transport position/speed
    LV2_Atom *beat = NULL, *bpm = NULL, *speed = NULL;
    lv2_atom_object_get(obj,
            uris->time_barBeat, &beat,
            uris->time_beatsPerMinute, &bpm,
            uris->time_speed, &speed,
            NULL);
    if (bpm && bpm->type == uris->atom_Float)
    {
        // Tempo changed, update BPM
        self->bpm = ((LV2_Atom_Float*)bpm)->body;
    }
    if (speed && speed->type == uris->atom_Float)
    {
        // Speed changed, e.g. 0 (stop) to 1 (play)
        self->speed = ((LV2_Atom_Float*)speed)->body;
    }
    if (beat && beat->type == uris->atom_Float)
    {
        // Received a beat position, synchronise
        self->beatInMeasure = ((LV2_Atom_Float*)beat)->body;
    }
}

static uint32_t
resetPhase(CvTime* self)
{
    uint32_t pos = (uint32_t)fmod(self->samplerate * (60.0f / self->bpm) * self->beatInMeasure, (self->samplerate * (60.0f / (self->bpm * self->divisions))));

    return pos;
}


static void
run(LV2_Handle instance, uint32_t n_samples)
{
    CvTime* self = (CvTime*)instance;

    const TimeURIs* uris = &self->uris;
    const LV2_Atom_Sequence* in     = self->control;

    for (const LV2_Atom_Event* ev = lv2_atom_sequence_begin(&in->body);
            !lv2_atom_sequence_is_end(&in->body, in->atom.size, ev);
            ev = lv2_atom_sequence_next(ev)) {

        if (ev->body.type == uris->atom_Object ||
                ev->body.type == uris->atom_Blank) {
            const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
            if (obj->body.otype == uris->time_Position) {
                update_position(self, obj);
            }
        }
    }


    for(uint32_t i = 0; i < n_samples; i ++) {
        // Map bpm to host or to bpm parameter
        if (!*self->sync) {
            self->bpm = *self->changeBpm;
        } else {
            self->bpm = self->bpm;
        }
        
        // Reset phase when playing starts or stops
        if (self->speed != self->prevSpeed) {
            self->pos = resetPhase(self);
            self->prevSpeed = self->speed;
        }
        
        // Reset phase when sync is turned on
        if (*self->sync != self->prevSync) {
            self->pos = resetPhase(self);
            self->prevSync = *self->sync;
        }
        
        // Reset phase when there is a new division
        if (self->divisions != *self->changedDiv) {
            self->divisions = *self->changedDiv;
            self->pos = resetPhase(self);
        }

        // Calculate period in samples for one division
        // period = (60 / bpm / divisions) * samplerate seconds
        self->period = (uint32_t)(self->samplerate * (60.0f / (self->bpm * self->divisions)));
        
        // Calculate current time within the division in milliseconds
        // time = (pos / period) * (period in milliseconds)
        float period_ms = (60000.0f / (self->bpm * self->divisions)); // Convert period to milliseconds
        self->current_time_ms = (float)self->pos / (float)self->period * period_ms;
        
        // Map time to CV output (0V to 10V)
        // Get min and max time values (in milliseconds)
        float min_time = *self->minTime;
        float max_time = *self->maxTime;
        
        // Clamp current time within min-max range and map to 0-10V
        float cv_value = 0.0f;
        if (max_time > min_time) {
            // Linear interpolation from min_time (0V) to max_time (10V)
            cv_value = ((self->current_time_ms - min_time) / (max_time - min_time)) * 10.0f;
            // Clamp to 0-10V range
            if (cv_value < 0.0f) cv_value = 0.0f;
            if (cv_value > 10.0f) cv_value = 10.0f;
        }
        
        self->cvOutput[i] = cv_value;
        self->pos += 1;
        
        // Reset position when period completes
        if (self->pos >= self->period) {
            self->pos = 0;
        }
    }
}

static void
deactivate(LV2_Handle instance)
{
}

static void
cleanup(LV2_Handle instance)
{
	free(instance);
}

static const void*
extension_data(const char* uri)
{
	return NULL;
}

static const LV2_Descriptor descriptor = {
    PLUGIN_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
	switch (index) {
	case 0:  return &descriptor;
	default: return NULL;
	}
}
