/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>

#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winerror.h>
#include <propidl.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <audioclient.h>

#ifdef _MSC_VER
DEFINE_GUID(IID_IAudioClient, 0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioRenderClient, 0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
DEFINE_GUID(IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
#undef KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
#endif

#include <lists/string_list.h>
#include <queues/fifo_queue.h>

#include "../../retroarch.h"
#include "../../verbosity.h"
#include "../../configuration.h"

DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14); /* DEVPROP_TYPE_STRING */

#ifdef __cplusplus
#define _IMMDeviceCollection_Item(This,nDevice,ppdevice) (This)->Item(nDevice,ppdevice)
#define _IAudioClient_Start(This)	( (This)->Start() )
#define _IAudioClient_Stop(This)	( (This)->Stop() )
#define _IAudioClient_GetCurrentPadding(This,pNumPaddingFrames)	\
    ( (This)->GetCurrentPadding(pNumPaddingFrames) )
#define _IAudioRenderClient_GetBuffer(This,NumFramesRequested,ppData)	\
    ( (This)->GetBuffer(NumFramesRequested,ppData) )
#define _IAudioRenderClient_ReleaseBuffer(This,NumFramesWritten,dwFlags)	\
    ( (This)->ReleaseBuffer(NumFramesWritten,dwFlags) )
#define _IAudioClient_GetService(This,riid,ppv) ( (This)->GetService(riid,ppv) )
#define _IAudioClient_SetEventHandle(This,eventHandle)	( (This)->SetEventHandle(eventHandle) )
#define _IAudioClient_GetBufferSize(This,pNumBufferFrames) ( (This)->GetBufferSize(pNumBufferFrames) )
#define _IAudioClient_GetStreamLatency(This,phnsLatency)	( (This)->GetStreamLatency(phnsLatency) )
#define _IAudioClient_GetDevicePeriod(This,phnsDefaultDevicePeriod,phnsMinimumDevicePeriod)	( (This)->GetDevicePeriod(phnsDefaultDevicePeriod,phnsMinimumDevicePeriod) )
#define _IMMDevice_Activate(This,iid,dwClsCtx,pActivationParams,ppv) ((This)->Activate(iid,(dwClsCtx),pActivationParams,ppv))
#define _IMMDeviceEnumerator_EnumAudioEndpoints(This,dataFlow,dwStateMask,ppDevices) (This)->EnumAudioEndpoints(dataFlow,dwStateMask,ppDevices)
#define _IMMDeviceEnumerator_GetDefaultAudioEndpoint(This,dataFlow,role,ppEndpoint) (This)->GetDefaultAudioEndpoint(dataFlow,role,ppEndpoint)
#define _IMMDevice_OpenPropertyStore(This,stgmAccess,ppProperties) (This)->OpenPropertyStore(stgmAccess,ppProperties)
#define _IMMDevice_GetId(This,ppstrId) ((This)->GetId(ppstrId))
#define _IPropertyStore_GetValue(This,key,pv) ( (This)->GetValue(key,pv) )
#define _IMMDeviceCollection_GetCount(This,cProps) ( (This)->GetCount(cProps) )
#else
#define _IMMDeviceCollection_Item(This,nDevice,ppdevice) (This)->lpVtbl->Item(This,nDevice,ppdevice)
#define _IAudioClient_Start(This)	( (This)->lpVtbl -> Start(This) )
#define _IAudioClient_Stop(This)	( (This)->lpVtbl -> Stop(This) )
#define _IAudioClient_GetCurrentPadding(This,pNumPaddingFrames)	\
    ( (This)->lpVtbl -> GetCurrentPadding(This,pNumPaddingFrames) )
#define _IAudioRenderClient_GetBuffer(This,NumFramesRequested,ppData)	\
    ( (This)->lpVtbl -> GetBuffer(This,NumFramesRequested,ppData) )
#define _IAudioRenderClient_ReleaseBuffer(This,NumFramesWritten,dwFlags)	\
    ( (This)->lpVtbl -> ReleaseBuffer(This,NumFramesWritten,dwFlags) )
#define _IAudioClient_GetService(This,riid,ppv)	( (This)->lpVtbl -> GetService(This,&(riid),ppv) )
#define _IAudioClient_SetEventHandle(This,eventHandle)	( (This)->lpVtbl -> SetEventHandle(This,eventHandle) )
#define _IAudioClient_GetBufferSize(This,pNumBufferFrames) ( (This)->lpVtbl -> GetBufferSize(This,pNumBufferFrames) )
#define _IAudioClient_GetStreamLatency(This,phnsLatency)	( (This)->lpVtbl -> GetStreamLatency(This,phnsLatency) )
#define _IAudioClient_GetDevicePeriod(This,phnsDefaultDevicePeriod,phnsMinimumDevicePeriod)	( (This)->lpVtbl -> GetDevicePeriod(This,phnsDefaultDevicePeriod,phnsMinimumDevicePeriod) )
#define _IMMDevice_Activate(This,iid,dwClsCtx,pActivationParams,ppv) ((This)->lpVtbl->Activate(This,&(iid),dwClsCtx,pActivationParams,ppv))
#define _IMMDeviceEnumerator_EnumAudioEndpoints(This,dataFlow,dwStateMask,ppDevices) (This)->lpVtbl->EnumAudioEndpoints(This,dataFlow,dwStateMask,ppDevices)
#define _IMMDeviceEnumerator_GetDefaultAudioEndpoint(This,dataFlow,role,ppEndpoint) (This)->lpVtbl->GetDefaultAudioEndpoint(This,dataFlow,role,ppEndpoint)
#define _IMMDevice_OpenPropertyStore(This,stgmAccess,ppProperties) (This)->lpVtbl->OpenPropertyStore(This,stgmAccess,ppProperties)
#define _IMMDevice_GetId(This,ppstrId) (This)->lpVtbl->GetId(This,ppstrId)
#define _IPropertyStore_GetValue(This,key,pv) ( (This)->lpVtbl -> GetValue(This,&(key),pv) )
#define _IMMDeviceCollection_GetCount(This,cProps) ( (This)->lpVtbl -> GetCount(This,cProps) )
#endif

#ifdef __cplusplus
#define IFACE_RELEASE(iface) \
      if (iface) \
      { \
         iface->Release(); \
         iface = NULL; \
      }
#else
#define IFACE_RELEASE(iface) \
      if (iface) \
      { \
         iface->lpVtbl->Release(iface);\
         iface = NULL; \
      }
#endif

typedef struct
{
   bool exclusive;
   bool blocking;
   bool running;
   size_t frame_size;     /* 4 or 8 only */
   size_t engine_buffer_size;
   HANDLE write_event;
   IMMDevice          *device;
   IAudioClient       *client;
   IAudioRenderClient *renderer;
   fifo_buffer_t      *buffer; /* NULL in unbuffered shared mode */
} wasapi_t;

static bool wasapi_check_device_id(IMMDevice *device, const char *id)
{
   HRESULT hr;
   bool result   = false;
   LPWSTR dev_id = NULL, dev_cmp_id = NULL;
   int id_length = MultiByteToWideChar(CP_ACP, 0, id, -1, NULL, 0);

   if (!(id_length > 0))
      goto error;

   dev_cmp_id = (LPWSTR)malloc(id_length * sizeof(WCHAR));
   if (!dev_cmp_id)
      goto error;

   id_length = MultiByteToWideChar(CP_ACP, 0, id, -1, dev_cmp_id, id_length);
   if (!(id_length > 0))
      goto error;

   hr = _IMMDevice_GetId(device, &dev_id);
   if (FAILED(hr))
      goto error;

   result = lstrcmpW(dev_cmp_id, dev_id) == 0 ? true : false;

   if (dev_id)
      CoTaskMemFree(dev_id);
   if (dev_cmp_id)
      free(dev_cmp_id);
   dev_id     = NULL;
   dev_cmp_id = NULL;

   return result;

error:
   if (dev_id)
      CoTaskMemFree(dev_id);
   if (dev_cmp_id)
      free(dev_cmp_id);
   dev_id     = NULL;
   dev_cmp_id = NULL;

   return false;
}

static IMMDevice *wasapi_init_device(const char *id)
{
   HRESULT hr;
   UINT32 dev_count, i;
   IMMDeviceEnumerator *enumerator = NULL;
   IMMDevice *device               = NULL;
   IMMDeviceCollection *collection = NULL;

   if (id)
   {
      RARCH_LOG("[WASAPI]: Initializing device %s ...\n", id);
   }
   else
   {
      RARCH_LOG("[WASAPI]: Initializing default device.. \n");
   }

#ifdef __cplusplus
   hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
         IID_IMMDeviceEnumerator, (void **)&enumerator);
#else
   hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
         &IID_IMMDeviceEnumerator, (void **)&enumerator);
#endif
   if (FAILED(hr))
      goto error;

   if (id)
   {
      hr = _IMMDeviceEnumerator_EnumAudioEndpoints(enumerator,
            eRender, DEVICE_STATE_ACTIVE, &collection);
      if (FAILED(hr))
         goto error;

      hr = _IMMDeviceCollection_GetCount(collection, &dev_count);
      if (FAILED(hr))
         goto error;

      for (i = 0; i < dev_count; ++i)
      {
         hr = _IMMDeviceCollection_Item(collection, i, &device);
         if (FAILED(hr))
            goto error;

         if (wasapi_check_device_id(device, id))
            break;

         IFACE_RELEASE(device);
      }
   }
   else
   {
      hr = _IMMDeviceEnumerator_GetDefaultAudioEndpoint(
            enumerator, eRender, eConsole, &device);
      if (FAILED(hr))
         goto error;
   }

   if (!device)
      goto error;

   IFACE_RELEASE(collection);
   IFACE_RELEASE(enumerator);

   return device;

error:
   IFACE_RELEASE(collection);
   IFACE_RELEASE(enumerator);

   if (id)
   {
      RARCH_WARN("[WASAPI]: Failed to initialize device.\n");
   }
   else
   {
      RARCH_ERR("[WASAPI]: Failed to initialize device.\n");
   }

   return NULL;
}

static unsigned wasapi_pref_rate(unsigned i)
{
   const unsigned r[] = { 48000, 44100, 96000, 192000 };

   if (i >= sizeof(r) / sizeof(unsigned))
      return 0;

   return r[i];
}

static void wasapi_set_format(WAVEFORMATEXTENSIBLE *wf,
      bool float_fmt, unsigned rate)
{
   wf->Format.nChannels               = 2;
   wf->Format.nSamplesPerSec          = rate;

   if (float_fmt)
   {
      wf->Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
      wf->Format.nAvgBytesPerSec      = rate * 8;
      wf->Format.nBlockAlign          = 8;
      wf->Format.wBitsPerSample       = 32;
      wf->Format.cbSize               = sizeof(WORD) + sizeof(DWORD) + sizeof(GUID);
      wf->Samples.wValidBitsPerSample = 32;
      wf->dwChannelMask               = KSAUDIO_SPEAKER_STEREO;
      wf->SubFormat                   = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
   }
   else
   {
      wf->Format.wFormatTag           = WAVE_FORMAT_PCM;
      wf->Format.nAvgBytesPerSec      = rate * 4;
      wf->Format.nBlockAlign          = 4;
      wf->Format.wBitsPerSample       = 16;
      wf->Format.cbSize               = 0;
   }
}

static IAudioClient *wasapi_init_client_sh(IMMDevice *device,
      bool *float_fmt, unsigned *rate, unsigned latency)
{
   WAVEFORMATEXTENSIBLE wf;
   int i, j;
   IAudioClient *client = NULL;
   bool float_fmt_res   = *float_fmt;
   unsigned rate_res    = *rate;
   HRESULT hr           = _IMMDevice_Activate(device,
         IID_IAudioClient,
         CLSCTX_ALL, NULL, (void**)&client);
   if (FAILED(hr))
      return NULL;

   /* once for float, once for pcm (requested first) */
   for (i = 0; i < 2; ++i)
   {
      rate_res = *rate;
      if (i == 1)
         float_fmt_res = !float_fmt_res;

      /* for requested rate (first) and all preferred rates */
      for (j = 0; rate_res; ++j)
      {
         RARCH_LOG("[WASAPI]: Initializing client (shared, %s, %uHz, %ums) ...\n",
               float_fmt_res ? "float" : "pcm", rate_res, latency);

         wasapi_set_format(&wf, float_fmt_res, rate_res);
#ifdef __cplusplus
         hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
               AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
               0, 0, (WAVEFORMATEX*)&wf, NULL);
#else
         hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_SHARED,
               AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
               0, 0, (WAVEFORMATEX*)&wf, NULL);
#endif

         if (hr == AUDCLNT_E_ALREADY_INITIALIZED)
         {
            HRESULT hr;
            IFACE_RELEASE(client);
            hr           = _IMMDevice_Activate(device,
                  IID_IAudioClient,
                  CLSCTX_ALL, NULL, (void**)&client);
            if (FAILED(hr))
               return NULL;

#ifdef __cplusplus
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                  0, 0, (WAVEFORMATEX*)&wf, NULL);
#else
            hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                  0, 0, (WAVEFORMATEX*)&wf, NULL);
#endif
         }
         if (hr != AUDCLNT_E_UNSUPPORTED_FORMAT)
         {
            i = 2; /* break from outer loop too */
            break;
         }

         RARCH_WARN("[WASAPI]: Unsupported format.\n");
         rate_res = wasapi_pref_rate(j);
         if (rate_res == *rate) /* requested rate is allready tested */
            rate_res = wasapi_pref_rate(++j); /* skip it */
      }
   }

   if (FAILED(hr))
      goto error;

   *float_fmt = float_fmt_res;
   *rate      = rate_res;

   return client;

error:
   IFACE_RELEASE(client);

   return NULL;
}

static IAudioClient *wasapi_init_client_ex(IMMDevice *device,
      bool *float_fmt, unsigned *rate, unsigned latency)
{
   WAVEFORMATEXTENSIBLE wf;
   int i, j;
   IAudioClient *client           = NULL;
   bool float_fmt_res             = *float_fmt;
   unsigned rate_res              = *rate;
   REFERENCE_TIME minimum_period  = 0;
   REFERENCE_TIME buffer_duration = 0;
   UINT32 buffer_length           = 0;
   HRESULT hr                     = _IMMDevice_Activate(device,
         IID_IAudioClient,
         CLSCTX_ALL, NULL, (void**)&client);
   if (FAILED(hr))
      return NULL;

   hr = _IAudioClient_GetDevicePeriod(client, NULL, &minimum_period);
   if (FAILED(hr))
      goto error;

   /* buffer_duration is in 100ns units */
   buffer_duration = latency * 10000.0;
   if (buffer_duration < minimum_period)
      buffer_duration = minimum_period;

   /* once for float, once for pcm (requested first) */
   for (i = 0; i < 2; ++i)
   {
      rate_res = *rate;
      if (i == 1)
         float_fmt_res = !float_fmt_res;

      /* for requested rate (first) and all preferred rates */
      for (j = 0; rate_res; ++j)
      {
         RARCH_LOG("[WASAPI]: Initializing client (exclusive, %s, %uHz, %ums) ...\n",
               float_fmt_res ? "float" : "pcm", rate_res, latency);

         wasapi_set_format(&wf, float_fmt_res, rate_res);
#ifdef __cplusplus
         hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
               AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
               buffer_duration, buffer_duration, (WAVEFORMATEX*)&wf, NULL);
#else
         hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
               AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
               buffer_duration, buffer_duration, (WAVEFORMATEX*)&wf, NULL);
#endif
         if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
         {
            hr = _IAudioClient_GetBufferSize(client, &buffer_length);
            if (FAILED(hr))
               goto error;

            IFACE_RELEASE(client);
            hr                     = _IMMDevice_Activate(device,
                  IID_IAudioClient,
                  CLSCTX_ALL, NULL, (void**)&client);
            if (FAILED(hr))
               return NULL;

            buffer_duration = 10000.0 * 1000.0 / rate_res * buffer_length + 0.5;
#ifdef __cplusplus
            hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                  buffer_duration, buffer_duration, (WAVEFORMATEX*)&wf, NULL);
#else
            hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                  buffer_duration, buffer_duration, (WAVEFORMATEX*)&wf, NULL);
#endif
         }
         if (hr == AUDCLNT_E_ALREADY_INITIALIZED)
         {
            IFACE_RELEASE(client);
            hr                     = _IMMDevice_Activate(device,
                  IID_IAudioClient,
                  CLSCTX_ALL, NULL, (void**)&client);
            if (FAILED(hr))
               return NULL;

#ifdef __cplusplus
            hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                  buffer_duration, buffer_duration, (WAVEFORMATEX*)&wf, NULL);
#else
            hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                  buffer_duration, buffer_duration, (WAVEFORMATEX*)&wf, NULL);
#endif
         }
         if (hr != AUDCLNT_E_UNSUPPORTED_FORMAT)
         {
            if (hr == AUDCLNT_E_DEVICE_IN_USE)
               goto error;

            if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
               goto error;

            i = 2; /* break from outer loop too */
            break;
         }

         RARCH_WARN("[WASAPI]: Unsupported format.\n");
         rate_res = wasapi_pref_rate(j);
         if (rate_res == *rate) /* requested rate is allready tested */
            rate_res = wasapi_pref_rate(++j); /* skip it */
      }
   }

   if (FAILED(hr))
      goto error;

   *float_fmt = float_fmt_res;
   *rate      = rate_res;

   return client;

error:
   IFACE_RELEASE(client);

   return NULL;
}

static IAudioClient *wasapi_init_client(IMMDevice *device, bool *exclusive,
      bool *float_fmt, unsigned *rate, unsigned latency)
{
   HRESULT hr;
   IAudioClient *client;
   double latency_res;
   REFERENCE_TIME device_period  = 0;
   REFERENCE_TIME stream_latency = 0;
   UINT32 buffer_length          = 0;

   if (*exclusive)
   {
      client = wasapi_init_client_ex(device, float_fmt, rate, latency);
      if (!client)
      {
         client = wasapi_init_client_sh(device, float_fmt, rate, latency);
         if (client)
            *exclusive = false;
      }
   }
   else
   {
      client = wasapi_init_client_sh(device, float_fmt, rate, latency);
      if (!client)
      {
         client = wasapi_init_client_ex(device, float_fmt, rate, latency);
         if (client)
            *exclusive = true;
      }
   }

   if (!client)
      return NULL;

   /* next calls are allowed to fail (we losing info only) */

   if (*exclusive)
      hr = _IAudioClient_GetDevicePeriod(client, NULL, &device_period);
   else
      hr = _IAudioClient_GetDevicePeriod(client, &device_period, NULL);

   if (FAILED(hr))
   {
      RARCH_WARN("[WASAPI]: IAudioClient::GetDevicePeriod failed with error 0x%.8X.\n", hr);
   }

   if (!*exclusive)
   {
      hr = _IAudioClient_GetStreamLatency(client, &stream_latency);
      if (FAILED(hr))
      {
         RARCH_WARN("[WASAPI]: IAudioClient::GetStreamLatency failed with error 0x%.8X.\n", hr);
      }
   }

   hr = _IAudioClient_GetBufferSize(client, &buffer_length);
   if (FAILED(hr))
   {
      RARCH_WARN("[WASAPI]: IAudioClient::GetBufferSize failed with error 0x%.8X.\n", hr);
   }

   if (*exclusive)
      latency_res = (double)buffer_length * 1000.0 / (*rate);
   else
      latency_res = (double)(stream_latency + device_period) / 10000.0;

   RARCH_LOG("[WASAPI]: Client initialized (%s, %s, %uHz, %.1fms).\n",
         *exclusive ? "exclusive" : "shared",
         *float_fmt ? "float" : "pcm", *rate, latency_res);

   RARCH_LOG("[WASAPI]: Client's buffer length is %u frames (%.1fms).\n",
         buffer_length, (double)buffer_length * 1000.0 / (*rate));

   RARCH_LOG("[WASAPI]: Device period is %.1fms (%lld frames).\n",
         (double)device_period / 10000.0, device_period * (*rate) / 10000000);

   return client;
}

static void *wasapi_init(const char *dev_id, unsigned rate, unsigned latency,
      unsigned u1, unsigned *u2)
{
   HRESULT hr;
   UINT32 frame_count        = 0;
   REFERENCE_TIME dev_period = 0;
   BYTE *dest                = NULL;
   settings_t *settings      = config_get_ptr();
   bool float_format         = settings->bools.audio_wasapi_float_format;
   int sh_buffer_length      = settings->ints.audio_wasapi_sh_buffer_length;
   wasapi_t *w               = (wasapi_t*)calloc(1, sizeof(wasapi_t));

   if (!w)
      return NULL;

   w->exclusive              = settings->bools.audio_wasapi_exclusive_mode;
   w->device                 = wasapi_init_device(dev_id);
   if (!w->device && dev_id)
      w->device = wasapi_init_device(NULL);
   if (!w->device)
      goto error;

   w->client = wasapi_init_client(w->device,
         &w->exclusive, &float_format, &rate, latency);
   if (!w->client)
      goto error;

   hr = _IAudioClient_GetBufferSize(w->client, &frame_count);
   if (FAILED(hr))
      goto error;

   w->frame_size         = float_format ? 8 : 4;
   w->engine_buffer_size = frame_count * w->frame_size;

   if (w->exclusive)
   {
      w->buffer = fifo_new(w->engine_buffer_size);
      if (!w->buffer)
         goto error;

      RARCH_LOG("[WASAPI]: Intermediate buffer length is %u frames (%.1fms).\n",
            frame_count, (double)frame_count * 1000.0 / rate);
   }
   else if (sh_buffer_length)
   {
      if (sh_buffer_length < 0)
      {
         hr = _IAudioClient_GetDevicePeriod(w->client, &dev_period, NULL);
         if (FAILED(hr))
            goto error;

         sh_buffer_length = dev_period * rate / 10000000;
      }

      w->buffer = fifo_new(sh_buffer_length * w->frame_size);
      if (!w->buffer)
         goto error;

      RARCH_LOG("[WASAPI]: Intermediate buffer length is %u frames (%.1fms).\n",
            sh_buffer_length, (double)sh_buffer_length * 1000.0 / rate);
   }
   else
   {
      RARCH_LOG("[WASAPI]: Intermediate buffer is off. \n");
   }

   w->write_event = CreateEventA(NULL, FALSE, FALSE, NULL);
   if (!w->write_event)
      goto error;

   hr = _IAudioClient_SetEventHandle(w->client, w->write_event);
   if (FAILED(hr))
      goto error;

   hr = _IAudioClient_GetService(w->client,
         IID_IAudioRenderClient, (void**)&w->renderer);
   if (FAILED(hr))
      goto error;

   hr = _IAudioRenderClient_GetBuffer(w->renderer, frame_count, &dest);
   if (FAILED(hr))
      goto error;

   hr = _IAudioRenderClient_ReleaseBuffer(
         w->renderer, frame_count,
         AUDCLNT_BUFFERFLAGS_SILENT);
   if (FAILED(hr))
      goto error;

   hr = _IAudioClient_Start(w->client);
   if (FAILED(hr))
      goto error;
   w->running  = true;
   w->blocking = settings->bools.audio_sync;

   return w;

error:
   IFACE_RELEASE(w->renderer);
   IFACE_RELEASE(w->client);
   IFACE_RELEASE(w->device);
   if (w->write_event)
      CloseHandle(w->write_event);
   if (w->buffer)
      fifo_free(w->buffer);
   free(w);

   return NULL;
}

static bool wasapi_flush(wasapi_t * w, const void * data, size_t size)
{
   BYTE *dest         = NULL;
   UINT32 frame_count = size / w->frame_size;
   HRESULT hr         = _IAudioRenderClient_GetBuffer(
         w->renderer, frame_count, &dest);
   if (FAILED(hr))
      return false;

   memcpy(dest, data, size);
   hr = _IAudioRenderClient_ReleaseBuffer(
         w->renderer, frame_count,
         0);
   if (FAILED(hr))
      return false;

   return true;
}

static bool wasapi_flush_buffer(wasapi_t * w, size_t size)
{
   BYTE *dest         = NULL;
   UINT32 frame_count = size / w->frame_size;
   HRESULT hr         = _IAudioRenderClient_GetBuffer(
         w->renderer, frame_count, &dest);
   if (FAILED(hr))
      return false;

   fifo_read(w->buffer, dest, size);
   hr = _IAudioRenderClient_ReleaseBuffer(
         w->renderer, frame_count,
         0);
   if (FAILED(hr))
      return false;

   return true;
}

static ssize_t wasapi_write_sh(wasapi_t *w, const void * data, size_t size)
{
   DWORD ir;
   HRESULT hr;
   size_t write_avail = 0;
   ssize_t written    = -1;
   UINT32 padding     = 0;

   if (w->buffer)
   {
      write_avail = fifo_write_avail(w->buffer);
      if (!write_avail)
      {
         size_t read_avail  = 0;

         if (w->blocking)
         {
            ir = WaitForSingleObject(w->write_event, INFINITE);
            if (!(ir == WAIT_OBJECT_0))
               return -1;
         }

         hr = _IAudioClient_GetCurrentPadding(w->client, &padding);
         if (FAILED(hr))
            return -1;

         read_avail  = fifo_read_avail(w->buffer);
         write_avail = w->engine_buffer_size - padding * w->frame_size;
         written     = read_avail < write_avail ? read_avail : write_avail;
         if (written)
            if (!wasapi_flush_buffer(w, written))
               return -1;
      }

      write_avail = fifo_write_avail(w->buffer);
      written     = size < write_avail ? size : write_avail;
      if (written)
         fifo_write(w->buffer, data, written);
   }
   else
   {
      if (w->blocking)
      {
         ir = WaitForSingleObject(w->write_event, INFINITE);
         if (!(ir == WAIT_OBJECT_0))
            return -1;
      }

      hr = _IAudioClient_GetCurrentPadding(w->client, &padding);
      if (FAILED(hr))
         return -1;

      write_avail = w->engine_buffer_size - padding * w->frame_size;
      if (!write_avail)
         return 0;

      written = size < write_avail ? size : write_avail;
      if (written)
         if (!wasapi_flush(w, data, written))
            return -1;
   }

   return written;
}

static ssize_t wasapi_write_ex(wasapi_t *w, const void * data, size_t size)
{
   ssize_t written    = 0;
   size_t write_avail = fifo_write_avail(w->buffer);

   if (!write_avail)
   {
      DWORD ir = WaitForSingleObject(
            w->write_event, w->blocking ? INFINITE : 0);
      if (ir != WAIT_OBJECT_0)
      {
         if (w->blocking)
            return -1;
         return 0;
      }

      if (!wasapi_flush_buffer(w, w->engine_buffer_size))
         return -1;

      write_avail = w->engine_buffer_size;
   }

   written = size < write_avail ? size : write_avail;
   fifo_write(w->buffer, data, written);

   return written;
}

static ssize_t wasapi_write(void *wh, const void *data, size_t size)
{
   size_t written;
   wasapi_t *w = (wasapi_t*)wh;

   if (w->blocking)
   {
      ssize_t ir;
      for (written = 0, ir = -1; written < size; written += ir)
      {
         if (w->exclusive)
            ir = wasapi_write_ex(w, (char*)data + written, size - written);
         else
            ir = wasapi_write_sh(w, (char*)data + written, size - written);
         if (ir == -1)
            return -1;
      }
   }
   else if (w->exclusive)
      written = wasapi_write_ex(w, data, size);
   else
      written = wasapi_write_sh(w, data, size);

   return written;
}

static bool wasapi_stop(void *wh)
{
   wasapi_t *w = (wasapi_t*)wh;
   HRESULT  hr = _IAudioClient_Stop(w->client);
   if (FAILED(hr))
      return !w->running;

   w->running = false;

   return true;
}

static bool wasapi_start(void *wh, bool u)
{
   wasapi_t *w = (wasapi_t*)wh;
   HRESULT  hr = _IAudioClient_Start(w->client);

   if (hr == AUDCLNT_E_NOT_STOPPED)
      return true;

   if (FAILED(hr))
      return w->running;

   w->running = true;

   return true;
}

static bool wasapi_alive(void *wh)
{
   wasapi_t *w = (wasapi_t*)wh;

   return w->running;
}

static void wasapi_set_nonblock_state(void *wh, bool nonblock)
{
   wasapi_t *w = (wasapi_t*)wh;

   RARCH_LOG("[WASAPI]: Sync %s.\n", nonblock ? "off" : "on");

   w->blocking = !nonblock;
}

static void wasapi_free(void *wh)
{
   DWORD ir;
   wasapi_t *w        = (wasapi_t*)wh;
   HANDLE write_event = w->write_event;

   IFACE_RELEASE(w->renderer);
   if (w->client)
      _IAudioClient_Stop(w->client);
   IFACE_RELEASE(w->client);
   IFACE_RELEASE(w->device);
   if (w->buffer)
      fifo_free(w->buffer);
   free(w);

   ir = WaitForSingleObject(write_event, 20);
   if (ir == WAIT_FAILED)
   {
      RARCH_ERR("[WASAPI]: WaitForSingleObject failed with error %d.\n", GetLastError());
   }

   /* If event isn't signaled log and leak */
   if (!(ir == WAIT_OBJECT_0))
      return;

   CloseHandle(write_event);
}

static bool wasapi_use_float(void *wh)
{
   wasapi_t *w = (wasapi_t*)wh;

   return w->frame_size == 8;
}

static void *wasapi_device_list_new(void *u)
{
   HRESULT hr;
   UINT i;
   PROPVARIANT prop_var;
   int ir;
   union string_list_elem_attr attr;
   IMMDeviceEnumerator *enumerator = NULL;
   IMMDeviceCollection *collection = NULL;
   UINT dev_count                  = 0;
   IMMDevice *device               = NULL;
   LPWSTR dev_id_wstr              = NULL;
   IPropertyStore *prop_store      = NULL;
   bool prop_var_init              = false;
   bool br                         = false;
   char *dev_id_str                = NULL;
   char *dev_name_str              = NULL;
   struct string_list *sl          = string_list_new();

   if (!sl)
      return NULL;

   attr.i = 0;
#ifdef __cplusplus
   hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
         IID_IMMDeviceEnumerator, (void **)&enumerator);
#else
   hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
         &IID_IMMDeviceEnumerator, (void **)&enumerator);
#endif
   if (FAILED(hr))
      goto error;

   hr = _IMMDeviceEnumerator_EnumAudioEndpoints(enumerator,
         eRender, DEVICE_STATE_ACTIVE, &collection);
   if (FAILED(hr))
      goto error;

   hr = _IMMDeviceCollection_GetCount(collection, &dev_count);
   if (FAILED(hr))
      goto error;

   for (i = 0; i < dev_count; ++i)
   {
      hr = _IMMDeviceCollection_Item(collection, i, &device);
      if (FAILED(hr))
         goto error;

      hr = _IMMDevice_GetId(device, &dev_id_wstr);
      if (FAILED(hr))
         goto error;

      ir = WideCharToMultiByte(CP_ACP, 0, dev_id_wstr, -1,
            NULL, 0, NULL, NULL);
      if (!ir)
         goto error;

      dev_id_str = (char *)malloc(ir);
      if (!dev_id_str)
         goto error;

      ir = WideCharToMultiByte(CP_ACP, 0, dev_id_wstr, -1,
            dev_id_str, ir, NULL, NULL);
      if (!ir)
         goto error;

      hr = _IMMDevice_OpenPropertyStore(device, STGM_READ, &prop_store);
      if (FAILED(hr))
         goto error;

      PropVariantInit(&prop_var);
      prop_var_init = true;
      hr = _IPropertyStore_GetValue(prop_store, PKEY_Device_FriendlyName,
            &prop_var);
      if (FAILED(hr))
         goto error;

      ir = WideCharToMultiByte(CP_ACP, 0, prop_var.pwszVal, -1,
            NULL, 0, NULL, NULL);
      if (!ir)
         goto error;

      dev_name_str = (char *)malloc(ir);
      if (!dev_name_str)
         goto error;

      ir = WideCharToMultiByte(CP_ACP, 0, prop_var.pwszVal, -1,
            dev_name_str, ir, NULL, NULL);
      if (!ir)
         goto error;

      RARCH_LOG("[WASAPI]: %s %s\n", dev_name_str, dev_id_str);

      br = string_list_append(sl, dev_id_str, attr);
      if (!br)
         goto error;

      PropVariantClear(&prop_var);
      prop_var_init = false;
      if (dev_id_wstr)
         CoTaskMemFree(dev_id_wstr);
      if (dev_id_str)
         free(dev_id_str);
      if (dev_name_str)
         free(dev_name_str);
      dev_id_str   = NULL;
      dev_name_str = NULL;
      dev_id_wstr = NULL;
      IFACE_RELEASE(prop_store);
      IFACE_RELEASE(device);
   }

   IFACE_RELEASE(collection);
   IFACE_RELEASE(enumerator);

   return sl;

error:
   if (dev_id_str)
      free(dev_id_str);
   if (dev_name_str)
      free(dev_name_str);
   dev_id_str   = NULL;
   dev_name_str = NULL;
   if (prop_var_init)
      PropVariantClear(&prop_var);
   IFACE_RELEASE(prop_store);
   if (dev_id_wstr)
      CoTaskMemFree(dev_id_wstr);
   dev_id_wstr = NULL;
   IFACE_RELEASE(device);
   IFACE_RELEASE(collection);
   IFACE_RELEASE(enumerator);
   if (sl)
      string_list_free(sl);

   RARCH_ERR("[WASAPI]: Device enumeration failed.\n");

   return NULL;
}

static void wasapi_device_list_free(void *u, void *slp)
{
   struct string_list *sl = (struct string_list*)slp;

   if (sl)
      string_list_free(sl);
}

static size_t wasapi_write_avail(void *wh)
{
   HRESULT hr;
   wasapi_t *w    = (wasapi_t*)wh;
   UINT32 padding = 0;

   if (w->buffer)
      return fifo_write_avail(w->buffer);

   hr = _IAudioClient_GetCurrentPadding(w->client, &padding);
   if (FAILED(hr))
      return 0;

   return w->engine_buffer_size - padding * w->frame_size;
}

static size_t wasapi_buffer_size(void *wh)
{
   wasapi_t *w = (wasapi_t*)wh;

   if (!w->exclusive && w->buffer)
      return w->buffer->size;

   return w->engine_buffer_size;
}

audio_driver_t audio_wasapi = {
   wasapi_init,
   wasapi_write,
   wasapi_stop,
   wasapi_start,
   wasapi_alive,
   wasapi_set_nonblock_state,
   wasapi_free,
   wasapi_use_float,
   "wasapi",
   wasapi_device_list_new,
   wasapi_device_list_free,
   wasapi_write_avail,
   wasapi_buffer_size
};
