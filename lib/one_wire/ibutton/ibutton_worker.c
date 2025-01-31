#include <furi.h>
#include <furi_hal.h>
#include <atomic.h>
#include "ibutton_worker_i.h"

typedef enum {
    iButtonMessageEnd,
    iButtonMessageStop,
    iButtonMessageRead,
    iButtonMessageWrite,
    iButtonMessageEmulate,
} iButtonMessageType;

typedef struct {
    iButtonMessageType type;
    union {
        iButtonKey* key;
    } data;
} iButtonMessage;

static int32_t ibutton_worker_thread(void* thread_context);

iButtonWorker* ibutton_worker_alloc() {
    iButtonWorker* worker = malloc(sizeof(iButtonWorker));
    worker->key_p = NULL;
    worker->key_data = malloc(ibutton_key_get_max_size());
    worker->host = onewire_host_alloc();
    worker->slave = onewire_slave_alloc();
    worker->writer = ibutton_writer_alloc(worker->host);
    worker->device = onewire_device_alloc(0, 0, 0, 0, 0, 0, 0, 0);
    worker->pulse_decoder = pulse_decoder_alloc();
    worker->protocol_cyfral = protocol_cyfral_alloc();
    worker->protocol_metakom = protocol_metakom_alloc();
    worker->messages = osMessageQueueNew(1, sizeof(iButtonMessage), NULL);
    worker->mode_index = iButtonWorkerIdle;
    worker->last_dwt_value = 0;
    worker->read_cb = NULL;
    worker->write_cb = NULL;
    worker->emulate_cb = NULL;
    worker->cb_ctx = NULL;

    worker->encoder_cyfral = encoder_cyfral_alloc();
    worker->encoder_metakom = encoder_metakom_alloc();

    worker->thread = furi_thread_alloc();
    furi_thread_set_name(worker->thread, "ibutton_worker");
    furi_thread_set_callback(worker->thread, ibutton_worker_thread);
    furi_thread_set_context(worker->thread, worker);
    furi_thread_set_stack_size(worker->thread, 2048);

    pulse_decoder_add_protocol(
        worker->pulse_decoder,
        protocol_cyfral_get_protocol(worker->protocol_cyfral),
        PulseProtocolCyfral);
    pulse_decoder_add_protocol(
        worker->pulse_decoder,
        protocol_metakom_get_protocol(worker->protocol_metakom),
        PulseProtocolMetakom);

    return worker;
}

void ibutton_worker_read_set_callback(
    iButtonWorker* worker,
    iButtonWorkerReadCallback callback,
    void* context) {
    furi_check(worker->mode_index == iButtonWorkerIdle);
    worker->read_cb = callback;
    worker->cb_ctx = context;
}

void ibutton_worker_write_set_callback(
    iButtonWorker* worker,
    iButtonWorkerWriteCallback callback,
    void* context) {
    furi_check(worker->mode_index == iButtonWorkerIdle);
    worker->write_cb = callback;
    worker->cb_ctx = context;
}

void ibutton_worker_emulate_set_callback(
    iButtonWorker* worker,
    iButtonWorkerEmulateCallback callback,
    void* context) {
    furi_check(worker->mode_index == iButtonWorkerIdle);
    worker->emulate_cb = callback;
    worker->cb_ctx = context;
}

void ibutton_worker_read_start(iButtonWorker* worker, iButtonKey* key) {
    iButtonMessage message = {.type = iButtonMessageRead, .data.key = key};
    furi_check(osMessageQueuePut(worker->messages, &message, 0, osWaitForever) == osOK);
}

void ibutton_worker_write_start(iButtonWorker* worker, iButtonKey* key) {
    iButtonMessage message = {.type = iButtonMessageWrite, .data.key = key};
    furi_check(osMessageQueuePut(worker->messages, &message, 0, osWaitForever) == osOK);
}

void ibutton_worker_emulate_start(iButtonWorker* worker, iButtonKey* key) {
    iButtonMessage message = {.type = iButtonMessageEmulate, .data.key = key};
    furi_check(osMessageQueuePut(worker->messages, &message, 0, osWaitForever) == osOK);
}

void ibutton_worker_stop(iButtonWorker* worker) {
    iButtonMessage message = {.type = iButtonMessageStop};
    furi_check(osMessageQueuePut(worker->messages, &message, 0, osWaitForever) == osOK);
}

void ibutton_worker_free(iButtonWorker* worker) {
    pulse_decoder_free(worker->pulse_decoder);
    protocol_metakom_free(worker->protocol_metakom);
    protocol_cyfral_free(worker->protocol_cyfral);

    ibutton_writer_free(worker->writer);

    onewire_slave_free(worker->slave);

    onewire_host_free(worker->host);
    onewire_device_free(worker->device);

    encoder_cyfral_free(worker->encoder_cyfral);
    encoder_metakom_free(worker->encoder_metakom);

    osMessageQueueDelete(worker->messages);

    furi_thread_free(worker->thread);
    free(worker->key_data);
    free(worker);
}

void ibutton_worker_start_thread(iButtonWorker* worker) {
    furi_thread_start(worker->thread);
}

void ibutton_worker_stop_thread(iButtonWorker* worker) {
    iButtonMessage message = {.type = iButtonMessageEnd};
    furi_check(osMessageQueuePut(worker->messages, &message, 0, osWaitForever) == osOK);
    furi_thread_join(worker->thread);
}

void ibutton_worker_switch_mode(iButtonWorker* worker, iButtonWorkerMode mode) {
    ibutton_worker_modes[worker->mode_index].stop(worker);
    worker->mode_index = mode;
    ibutton_worker_modes[worker->mode_index].start(worker);
}

void ibutton_worker_set_key_p(iButtonWorker* worker, iButtonKey* key) {
    worker->key_p = key;
}

static int32_t ibutton_worker_thread(void* thread_context) {
    iButtonWorker* worker = thread_context;
    bool running = true;
    iButtonMessage message;
    osStatus_t status;

    ibutton_worker_modes[worker->mode_index].start(worker);

    while(running) {
        status = osMessageQueueGet(
            worker->messages, &message, NULL, ibutton_worker_modes[worker->mode_index].quant);
        if(status == osOK) {
            switch(message.type) {
            case iButtonMessageEnd:
                ibutton_worker_switch_mode(worker, iButtonWorkerIdle);
                ibutton_worker_set_key_p(worker, NULL);
                running = false;
                break;
            case iButtonMessageStop:
                ibutton_worker_switch_mode(worker, iButtonWorkerIdle);
                ibutton_worker_set_key_p(worker, NULL);
                break;
            case iButtonMessageRead:
                ibutton_worker_set_key_p(worker, message.data.key);
                ibutton_worker_switch_mode(worker, iButtonWorkerRead);
                break;
            case iButtonMessageWrite:
                ibutton_worker_set_key_p(worker, message.data.key);
                ibutton_worker_switch_mode(worker, iButtonWorkerWrite);
                break;
            case iButtonMessageEmulate:
                ibutton_worker_set_key_p(worker, message.data.key);
                ibutton_worker_switch_mode(worker, iButtonWorkerEmulate);
                break;
            }
        } else if(status == osErrorTimeout) {
            ibutton_worker_modes[worker->mode_index].tick(worker);
        } else {
            furi_crash("iButton worker error");
        }
    }

    ibutton_worker_modes[worker->mode_index].stop(worker);

    return 0;
}
