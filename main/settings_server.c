/**
 * Settings Web Server - Implementation
 *
 * Serves a settings page on port 80 while connected to the local network.
 * Config changes are saved to NVS immediately.
 */

#include "settings_server.h"
#include "app_config.h"
#include "nvs_config.h"
#include "aic3204.h"
#include "media_radio.h"
#include "ws_client.h"
#include "led_control.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "settings";
static httpd_handle_t server = NULL;
static micimike_config_t *live_cfg = NULL;
static settings_changed_cb_t settings_changed_cb = NULL;
static char runtime_wakeword[32] = DEFAULT_WAKEWORD;
static bool runtime_wakeword_reboot_required = false;

static esp_err_t api_reboot_handler(httpd_req_t *req);

static bool get_wifi_rssi(int8_t *rssi)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        *rssi = ap.rssi;
        return true;
    }
    return false;
}

static void make_hostname(const char *name, char *out, size_t out_size)
{
    size_t pos = 0;
    const char *src = (name && name[0]) ? name : DEFAULT_DEVICE_NAME;
    for (size_t i = 0; src[i] && pos + 1 < out_size; i++) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[pos++] = c;
        } else if (c >= 'A' && c <= 'Z') {
            out[pos++] = (char)(c - 'A' + 'a');
        } else if (pos > 0 && out[pos - 1] != '-') {
            out[pos++] = '-';
        }
    }
    while (pos > 0 && out[pos - 1] == '-') pos--;
    if (pos == 0) {
        strncpy(out, "micimike", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    out[pos] = '\0';
}

static void normalize_device_name(char *name, size_t size)
{
    char input[33];
    strncpy(input, name && name[0] ? name : "", sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';

    const char *suffix = input;
    if (strncasecmp(input, DEFAULT_DEVICE_NAME, strlen(DEFAULT_DEVICE_NAME)) == 0) {
        suffix = input + strlen(DEFAULT_DEVICE_NAME);
        while (*suffix == ' ' || *suffix == '-' || *suffix == '_' || *suffix == ':') {
            suffix++;
        }
    }

    if (suffix[0] == '\0') {
        strncpy(name, DEFAULT_DEVICE_NAME, size - 1);
    } else {
        snprintf(name, size, "%s %s", DEFAULT_DEVICE_NAME, suffix);
    }
    name[size - 1] = '\0';
}

static void apply_hostname(const char *device_name)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return;

    char hostname[33];
    make_hostname(device_name, hostname, sizeof(hostname));
    esp_netif_set_hostname(netif, hostname);
}

static bool is_valid_realtime_voice(const char *voice)
{
    static const char *const valid[] = {
        "marin", "cedar", "alloy", "ash", "ballad",
        "coral", "echo", "sage", "shimmer", "verse",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (strcmp(voice, valid[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_valid_conversation_style(const char *style)
{
    static const char *const valid[] = {
        "default", "professional", "friendly", "honest",
        "quirky", "efficient", "cynical",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (strcmp(style, valid[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_valid_ui_language(const char *language)
{
    static const char *const valid[] = {
        "en", "de", "es", "pt", "it", "pl", "hu",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (strcmp(language, valid[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_valid_wakeword_sensitivity(const char *sensitivity)
{
    static const char *const valid[] = {
        "slight", "moderate", "very",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (strcmp(sensitivity, valid[i]) == 0) {
            return true;
        }
    }
    return false;
}

void settings_server_set_runtime_wakeword(const char *active_wakeword, bool reboot_required)
{
    strncpy(runtime_wakeword,
            active_wakeword && active_wakeword[0] ? active_wakeword : DEFAULT_WAKEWORD,
            sizeof(runtime_wakeword) - 1);
    runtime_wakeword[sizeof(runtime_wakeword) - 1] = '\0';
    runtime_wakeword_reboot_required = reboot_required;
}

// ---------------------------------------------------------------------------
// Settings page HTML
// ---------------------------------------------------------------------------
static const char SETTINGS_HTML[] =
"<!DOCTYPE html><html><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MiciMike Settings</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0;}"
"body{font-family:'Segoe UI',system-ui,sans-serif;background:#0d1117;color:#c9d1d9;padding:16px;}"
".wrap{max-width:520px;margin:0 auto;}"
".top{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;margin-bottom:10px;}"
"h1{color:#00d4aa;font-size:1.5em;margin-bottom:4px;}"
".sub{color:#8b949e;font-size:0.85em;margin-bottom:20px;}"
".telemetry{display:flex;gap:8px;align-items:flex-start;flex-wrap:wrap;justify-content:flex-end;}"
".quickbar{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin:0 0 12px;}"
".quickbar .langbox{grid-column:3;background:#161b22;border:1px solid #30363d;border-radius:6px;padding:8px 10px;}"
".quickbar label{margin:0 0 4px;font-size:0.72em;}"
".quickbar select{padding:6px 8px;font-size:0.82em;}"
"@media(max-width:560px){.quickbar{grid-template-columns:1fr}.quickbar .langbox{grid-column:auto;}}"
".rssi{color:#8b949e;background:#161b22;border:1px solid #30363d;border-radius:6px;"
"font-size:0.78em;padding:5px 8px;white-space:nowrap;margin-top:2px;}"
".usage{color:#8b949e;background:#161b22;border:1px solid #30363d;border-radius:6px;"
"font-size:0.78em;padding:5px 8px;white-space:nowrap;margin-top:2px;}"
".usage.ok{color:#c9d1d9;border-color:#3fb950;}"
".statusbar{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:12px;}"
".pill{display:flex;align-items:center;gap:7px;background:#161b22;border:1px solid #30363d;"
"border-radius:6px;padding:8px 10px;color:#c9d1d9;font-size:0.82em;font-weight:600;}"
".pill span:last-child{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
".led{width:10px;height:10px;border-radius:50%;background:#da3633;box-shadow:0 0 8px rgba(218,54,51,.45);}"
".led.ok{background:#3fb950;box-shadow:0 0 8px rgba(63,185,80,.55);}"
".card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:16px;margin-bottom:12px;}"
".card h2{color:#58a6ff;font-size:1em;margin-bottom:12px;border-bottom:1px solid #30363d;padding-bottom:8px;}"
"label{display:block;color:#8b949e;font-size:0.8em;margin:8px 0 4px;}"
"input,select,textarea{width:100%;padding:8px 10px;border:1px solid #30363d;border-radius:6px;"
"background:#0d1117;color:#c9d1d9;font-size:0.95em;font-family:monospace;}"
"input:focus,select:focus,textarea:focus{border-color:#58a6ff;outline:none;}"
"textarea{resize:vertical;min-height:60px;}"
".promptbox{white-space:pre-wrap;color:#8b949e;background:#111820;border:1px solid #30363d;border-radius:6px;padding:8px 10px;font-size:0.78em;line-height:1.35;}"
".row{display:flex;gap:8px;align-items:center;}"
".row input{flex:1;}"
".btn{display:inline-block;padding:8px 16px;border:none;border-radius:6px;"
"font-size:0.9em;cursor:pointer;font-weight:600;}"
".btn-primary{background:#00d4aa;color:#0d1117;width:100%;padding:10px;margin-top:8px;}"
".btn-primary:hover{background:#00b894;}"
".btn-danger{background:#da3633;color:#fff;width:100%;padding:10px;margin-top:8px;}"
".btn-danger:hover{background:#b62324;}"
".status{padding:10px;border-radius:6px;margin-top:8px;display:none;font-size:0.9em;}"
".status.ok{display:block;background:#0d2818;border:1px solid:#196c2e;color:#3fb950;}"
".status.err{display:block;background:#2d1117;border:1px solid #da3633;color:#f85149;}"
".tag{display:inline-block;background:#30363d;color:#8b949e;padding:2px 8px;"
"border-radius:12px;font-size:0.75em;margin-left:6px;}"
".info{color:#8b949e;font-size:0.78em;margin-top:4px;}"
"</style></head><body><div class='wrap'>"
"<div class='top'><div><h1 id='title'>&#9881; MiciMike</h1>"
"<p class='sub' id='ip'></p>"
"</div><div class='telemetry'><div class='usage' id='tok-text'>TOK --</div><div class='rssi' id='rssi'>RSSI -- dBm</div></div></div>"
"<div class='quickbar'><div class='langbox'><label id='ui-lang-label'>Web UI Language</label>"
"<select id='ui-language' onchange='saveUiLanguage()'>"
"<option value='hu'>Magyar</option>"
"<option value='en'>English</option>"
"<option value='de'>Deutsch</option>"
"<option value='es'>Espanol</option>"
"<option value='pt'>Portugues</option>"
"<option value='it'>Italiano</option>"
"<option value='pl'>Polski</option>"
"</select></div></div>"
"<div class='statusbar'>"
"<div class='pill'><span class='led' id='dac-led'></span><span id='dac-text'>DAC</span></div>"
"<div class='pill'><span class='led' id='pa-led'></span><span id='pa-text'>PA</span></div>"
"<div class='pill'><span class='led' id='mute-led'></span><span id='mute-text'>MUTE</span></div>"
"</div>"

// Volume & EQ card
"<div class='card'><h2>&#127926; Volume &amp; EQ</h2>"
"<label>Volume</label>"
"<div class='row'><input type='range' id='volume' min='0' max='100' step='5'>"
"<span id='vol-val'>30</span>%</div>"
"<div class='status' id='volume-status'></div>"
"<label>EQ Profile</label>"
"<select id='eq-profile' onchange='saveEq()'>"
"<option value='Flat'>Flat</option>"
"<option value='Rock'>Rock</option>"
"<option value='Pop'>Pop</option>"
"<option value='Voice'>Voice</option>"
"<option value='Clear'>Clear</option>"
"<option value='Loudness'>Loudness</option>"
"<option value='Jazz'>Jazz</option>"
"</select>"
"<p class='info'>Applied in the AIC3204 DAC, so it affects radio and voice playback.</p>"
"<div class='status' id='eq-status'></div>"
"</div>"

// API Configuration card
"<div class='card'><h2>&#128273; API Configuration</h2>"
"<label>API Key</label>"
"<input type='password' id='apikey' placeholder='sk-...' autocomplete='off' data-bwignore='true' spellcheck='false'>"
"<p class='info'>Paste your OpenAI (or compatible) API key here.</p>"
"<label>API Endpoint</label>"
"<input id='apiurl' placeholder='wss://api.openai.com/v1/realtime?model=gpt-realtime' autocomplete='off' data-bwignore='true' autocorrect='off' autocapitalize='off' spellcheck='false'>"
"<p class='info'>Change only for non-OpenAI endpoints.</p>"
"<label id='admin-key-label'>OpenAI Admin Key</label>"
"<input type='password' id='adminkey' placeholder='sk-admin-...' autocomplete='off' data-bwignore='true' spellcheck='false'>"
"<p class='info' id='admin-key-info'>Optional. Enables daily, weekly and monthly organization usage lookup.</p>"
"<button class='btn btn-primary' onclick='saveApi()'>Save API Settings</button>"
"<div class='status' id='api-status'></div>"
"</div>"

// Voice card
"<div class='card'><h2>&#127908; Voice</h2>"
"<label>Wake Word</label>"
"<select id='wakeword'>"
"<option value='okay_nabu'>Okay Nabu</option>"
"<option value='hey_jarvis'>Hey Jarvis</option>"
"<option value='hey_mycroft'>Hey Mycroft</option>"
"<option value='alexa'>Alexa</option>"
"</select>"
"<p class='info' id='wakeword-runtime'></p>"
"<label>Wake Word Sensitivity</label>"
"<select id='ww-sensitivity'>"
"<option value='slight'>Slightly sensitive</option>"
"<option value='moderate'>Moderately sensitive</option>"
"<option value='very'>Very sensitive</option>"
"</select>"
"<label id='voice-label'>Realtime Voice</label>"
"<select id='rt-voice'>"
"<option value='marin'>Marin (recommended)</option>"
"<option value='cedar'>Cedar (recommended)</option>"
"<option value='alloy'>Alloy</option>"
"<option value='ash'>Ash</option>"
"<option value='ballad'>Ballad</option>"
"<option value='coral'>Coral</option>"
"<option value='echo'>Echo</option>"
"<option value='sage'>Sage</option>"
"<option value='shimmer'>Shimmer</option>"
"<option value='verse'>Verse</option>"
"</select>"
"<label id='style-label'>Conversation Style</label>"
"<select id='conv-style'>"
"<option value='default'>Alapertelmezett</option>"
"<option value='professional'>Szakszeru</option>"
"<option value='friendly'>Baratsagos</option>"
"<option value='honest'>Oszinte</option>"
"<option value='quirky'>Kulonc</option>"
"<option value='efficient'>Hatekony</option>"
"<option value='cynical'>Cinikus</option>"
"</select>"
"<label>Built-in System Prompt</label>"
"<div id='base-prompt' class='promptbox'>You are the voice assistant inside a MiciMike device.\nDetect and follow the user's spoken language automatically.\nIf the user asks for real-time translation between languages, do that.\nUse device_control only for explicit radio and volume commands.\nFor current facts, dates, holidays, news, prices, or anything that may have changed, call web_lookup before answering.\nNever start radio playback unless the user's latest request explicitly asks for it.\nAfter a successful tool call, briefly confirm the action in the user's language.</div>"
"<label>Additional System Prompt</label>"
"<textarea id='system-prompt' maxlength='1000' placeholder='Optional custom instructions...' autocomplete='off' data-bwignore='true' autocorrect='off' autocapitalize='off' spellcheck='false'></textarea>"
"<label>Session idle timeout (seconds)</label>"
"<input type='number' id='session-timeout' min='3' max='300' step='1' autocomplete='off' data-bwignore='true'>"
"<p class='info'>After wake, the API session stays open until this many seconds pass without local speech.</p>"
"<button class='btn btn-primary' onclick='saveVoice()'>Save Voice Settings</button>"
"<div class='status' id='voice-status'></div>"
"</div>"

// Media card
"<div class='card'><h2>&#127911; Media</h2>"
"<label>Internet Radio</label>"
"<select id='radio-station'><option value='-1'>-- no stations --</option></select>"
"<p class='info'>Supports direct MP3, AAC, M4A, TS and WAV HTTP streams.</p>"
"<div class='row' style='margin-top:6px;'>"
"<button class='btn btn-primary' onclick='playRadio()'>Play</button>"
"<button class='btn btn-danger' onclick='stopRadio()'>Stop</button>"
"<button class='btn' style='background:#30363d;color:#c9d1d9;flex:0 0 auto;' onclick='toggleStationEditor()'>Edit</button>"
"</div>"
"<div id='radio-now' class='info' style='margin-top:6px;'></div>"
"<div class='status' id='radio-status'></div>"

"<div id='station-editor' style='display:none;margin-top:14px;padding:10px;border:1px solid #30363d;border-radius:6px;background:#0d1117;'>"
"<p class='info' style='margin-bottom:8px;'>Up to 8 stations. Leave name and URL both empty to remove a row.</p>"
"<div id='station-rows'></div>"
"<div class='row' style='margin-top:8px;'>"
"<button class='btn btn-primary' onclick='saveStations()'>Save Stations</button>"
"<button class='btn' style='background:#30363d;color:#c9d1d9;' onclick='toggleStationEditor()'>Close</button>"
"</div>"
"</div>"

"<label><input type='checkbox' id='sendspin' disabled> Enable Snapcast (Sendspin) client <span class='tag'>Later</span></label>"
"</div>"

// System card
"<div class='card'><h2>&#9881; System</h2>"
"<label>Device Name</label>"
"<div class='row'><span class='tag'>MiciMike</span><input id='device-name' maxlength='24' placeholder='Kitchen' autocomplete='off' data-bwignore='true' autocorrect='off' autocapitalize='off' spellcheck='false'></div>"
"<button class='btn btn-primary' onclick='saveDevice()'>Save Device Name</button>"
"<div class='status' id='device-status'></div>"
"<p class='info' id='fwinfo'></p>"
"<button class='btn btn-primary' onclick='rebootDevice()'>Restart Device</button>"
"<button class='btn btn-danger' onclick='factoryReset()'>Factory Reset</button>"
"</div>"

"<script>"
"function $(id){return document.getElementById(id);}"
"function show(id,msg,ok){var s=$(id);s.textContent=msg;s.className='status '+(ok?'ok':'err');}"
"function led(id,on){$(id).className='led '+(on?'ok':'');}"
"function suffixFromName(n){n=n||'';return n.toLowerCase().indexOf('micimike')===0?n.slice(8).replace(/^[\\s\\-_:\\.]+/,''):n;}"
"var lang='hu';var orgUsageActive=false;"

"var dict={"
"hu:{ui:'Web UI nyelv',vol:'Hanger\u0151',eq:'EQ profil',api:'API be\u00e1ll\u00edt\u00e1sok',apikey:'API kulcs',admin:'OpenAI Admin kulcs',adminInfo:'Opcion\u00e1lis. Napi, heti \u00e9s havi szervezeti tokenlek\u00e9rdez\u00e9shez kell.',voice:'Hang',wake:'Wake Word',rtvoice:'Realtime hang',style:'Besz\u00e9lget\u00e9si st\u00edlus',prompt:'Rendszer prompt',basePrompt:'Be\u00e9p\u00edtett rendszer prompt',addPrompt:'Kieg\u00e9sz\u00edt\u0151 rendszer prompt',timeout:'Session idle timeout (m\u00e1sodperc)',media:'M\u00e9dia',radio:'Internetes r\u00e1di\u00f3',play:'Lej\u00e1tsz\u00e1s',stop:'Le\u00e1ll\u00edt\u00e1s',edit:'Szerkeszt\u00e9s',system:'Rendszer',dev:'Eszk\u00f6zn\u00e9v',saveApi:'API ment\u00e9se',saveVoice:'Voice ment\u00e9se',saveDev:'Eszk\u00f6zn\u00e9v ment\u00e9se',reboot:'Eszk\u00f6z \\u00fajraind\\u00edt\\u00e1sa',reset:'Gy\u00e1ri vissza\u00e1ll\\u00edt\\u00e1s',styles:['Alap\u00e9rtelmezett','Szakszer\u0171','Bar\u00e1ts\u00e1gos','\u0150szinte','K\u00fcl\u00f6nc','Hat\u00e9kony','Cinikus']},"
"en:{ui:'Web UI language',vol:'Volume',eq:'EQ profile',api:'API settings',apikey:'API key',admin:'OpenAI Admin key',adminInfo:'Optional. Enables daily, weekly and monthly organization token lookup.',voice:'Voice',wake:'Wake Word',rtvoice:'Realtime voice',style:'Conversation style',prompt:'System prompt',basePrompt:'Built-in system prompt',addPrompt:'Additional system prompt',timeout:'Session idle timeout (seconds)',media:'Media',radio:'Internet radio',play:'Play',stop:'Stop',edit:'Edit',system:'System',dev:'Device name',saveApi:'Save API settings',saveVoice:'Save Voice settings',saveDev:'Save device name',reboot:'Restart device',reset:'Factory reset',styles:['Default','Professional','Friendly','Honest','Quirky','Efficient','Cynical']},"
"de:{ui:'Web-UI-Sprache',vol:'Lautst\u00e4rke',eq:'EQ-Profil',api:'API-Einstellungen',apikey:'API-Schl\u00fcssel',admin:'OpenAI Admin-Schl\u00fcssel',adminInfo:'Optional. Aktiviert t\u00e4gliche, w\u00f6chentliche und monatliche Token-Nutzung.',voice:'Stimme',wake:'Wake Word',rtvoice:'Realtime-Stimme',style:'Gespr\u00e4chsstil',prompt:'System-Prompt',basePrompt:'Eingebauter System-Prompt',addPrompt:'Zusatz-System-Prompt',timeout:'Session-Leerlaufzeit (Sekunden)',media:'Medien',radio:'Internetradio',play:'Start',stop:'Stopp',edit:'Bearbeiten',system:'System',dev:'Ger\u00e4tename',saveApi:'API speichern',saveVoice:'Voice speichern',saveDev:'Ger\u00e4tename speichern',reset:'Werkseinstellungen',styles:['Standard','Professionell','Freundlich','Ehrlich','Eigenwillig','Effizient','Zynisch']},"
"es:{ui:'Idioma de la interfaz',vol:'Volumen',eq:'Perfil EQ',api:'Ajustes API',apikey:'Clave API',admin:'Clave Admin OpenAI',adminInfo:'Opcional. Activa uso diario, semanal y mensual de tokens.',voice:'Voz',wake:'Wake Word',rtvoice:'Voz Realtime',style:'Estilo de conversaci\u00f3n',prompt:'Prompt del sistema',basePrompt:'Prompt del sistema integrado',addPrompt:'Prompt adicional del sistema',timeout:'Tiempo inactivo de sesi\u00f3n (segundos)',media:'Media',radio:'Radio por internet',play:'Reproducir',stop:'Detener',edit:'Editar',system:'Sistema',dev:'Nombre del dispositivo',saveApi:'Guardar API',saveVoice:'Guardar voz',saveDev:'Guardar nombre',reset:'Restablecer',styles:['Predeterminado','Profesional','Amable','Honesto','Exc\u00e9ntrico','Eficiente','C\u00ednico']},"
"pt:{ui:'Idioma da interface',vol:'Volume',eq:'Perfil EQ',api:'Defini\u00e7\u00f5es API',apikey:'Chave API',admin:'Chave Admin OpenAI',adminInfo:'Opcional. Ativa uso di\u00e1rio, semanal e mensal de tokens.',voice:'Voz',wake:'Wake Word',rtvoice:'Voz Realtime',style:'Estilo de conversa',prompt:'Prompt do sistema',basePrompt:'Prompt de sistema integrado',addPrompt:'Prompt adicional do sistema',timeout:'Timeout da sess\u00e3o (segundos)',media:'Media',radio:'R\u00e1dio internet',play:'Reproduzir',stop:'Parar',edit:'Editar',system:'Sistema',dev:'Nome do dispositivo',saveApi:'Guardar API',saveVoice:'Guardar voz',saveDev:'Guardar nome',reset:'Reposi\u00e7\u00e3o de f\u00e1brica',styles:['Padr\u00e3o','Profissional','Amig\u00e1vel','Honesto','Exc\u00eantrico','Eficiente','C\u00ednico']},"
"it:{ui:'Lingua interfaccia',vol:'Volume',eq:'Profilo EQ',api:'Impostazioni API',apikey:'Chiave API',admin:'Chiave Admin OpenAI',adminInfo:'Opzionale. Abilita uso token giornaliero, settimanale e mensile.',voice:'Voce',wake:'Wake Word',rtvoice:'Voce Realtime',style:'Stile conversazione',prompt:'Prompt di sistema',basePrompt:'Prompt di sistema integrato',addPrompt:'Prompt di sistema aggiuntivo',timeout:'Timeout sessione (secondi)',media:'Media',radio:'Radio internet',play:'Riproduci',stop:'Stop',edit:'Modifica',system:'Sistema',dev:'Nome dispositivo',saveApi:'Salva API',saveVoice:'Salva voce',saveDev:'Salva nome',reset:'Ripristino',styles:['Predefinito','Professionale','Amichevole','Onesto','Eccentrico','Efficiente','Cinico']},"
"pl:{ui:'J\u0119zyk interfejsu',vol:'G\u0142o\u015bno\u015b\u0107',eq:'Profil EQ',api:'Ustawienia API',apikey:'Klucz API',admin:'Klucz Admin OpenAI',adminInfo:'Opcjonalnie. W\u0142\u0105cza dzienne, tygodniowe i miesi\u0119czne u\u017cycie token\u00f3w.',voice:'G\u0142os',wake:'Wake Word',rtvoice:'G\u0142os Realtime',style:'Styl rozmowy',prompt:'Prompt systemowy',basePrompt:'Wbudowany prompt systemowy',addPrompt:'Dodatkowy prompt systemowy',timeout:'Limit bezczynno\u015bci sesji (sekundy)',media:'Media',radio:'Radio internetowe',play:'Odtw\u00f3rz',stop:'Stop',edit:'Edytuj',system:'System',dev:'Nazwa urz\u0105dzenia',saveApi:'Zapisz API',saveVoice:'Zapisz g\u0142os',saveDev:'Zapisz nazw\u0119',reset:'Reset fabryczny',styles:['Domy\u015blny','Profesjonalny','Przyjazny','Szczery','Ekscentryczny','Wydajny','Cyniczny']}"
"};"
"var sensLabel={hu:'Wake Word \\u00e9rz\\u00e9kenys\\u00e9g',en:'Wake Word sensitivity',de:'Wake-Word-Empfindlichkeit',es:'Sensibilidad Wake Word',pt:'Sensibilidade Wake Word',it:'Sensibilit\\u00e0 Wake Word',pl:'Czu\\u0142o\\u015b\\u0107 Wake Word'};"
"var sensOpts={hu:['Enyh\\u00e9n \\u00e9rz\\u00e9keny','K\\u00f6zepesen \\u00e9rz\\u00e9keny','Nagyon \\u00e9rz\\u00e9keny'],en:['Slightly sensitive','Moderately sensitive','Very sensitive'],de:['Leicht empfindlich','Mittel empfindlich','Sehr empfindlich'],es:['Ligeramente sensible','Moderadamente sensible','Muy sensible'],pt:['Pouco sens\\u00edvel','Moderadamente sens\\u00edvel','Muito sens\\u00edvel'],it:['Leggermente sensibile','Moderatamente sensibile','Molto sensibile'],pl:['Lekko czu\\u0142e','Umiarkowanie czu\\u0142e','Bardzo czu\\u0142e']};"
"var uiWords={hu:{connected:'Kapcsol\\u00f3dva',playing:'Sz\\u00f3l',connecting:'Kapcsol\\u00f3d\\u00e1s',stopped:'Le\\u00e1ll\\u00edtva',error:'Hiba',selectStation:'V\\u00e1lassz \\u00e1llom\\u00e1st',saved:'Mentve',volSaved:'Hanger\\u0151 mentve',eqSaved:'EQ mentve',stationsSaved:'\\u00c1llom\\u00e1sok mentve',noStations:'nincs \\u00e1llom\\u00e1s, Edit',activeWake:'Akt\\u00edv wakeword',rebootNeeded:'a v\\u00e1lt\\u00e1s \\u00fajraind\\u00edt\\u00e1s ut\\u00e1n l\\u00e9p \\u00e9letbe',confirmReset:'T\\u00f6r\\u00f6ljek minden be\\u00e1ll\\u00edt\\u00e1st \\u00e9s \\u00fajrainduljak?',resetting:'\\u00dajraind\\u00edt\\u00e1s...',rebootSetup:'Az eszk\\u00f6z setup m\\u00f3dba indul.'},en:{connected:'Connected',playing:'Playing',connecting:'Connecting',stopped:'Stopped',error:'Error',selectStation:'Select a station first',saved:'Saved',volSaved:'Volume saved',eqSaved:'EQ saved',stationsSaved:'Stations saved',noStations:'no stations, Edit',activeWake:'Active wake word',rebootNeeded:'change applies after reboot',confirmReset:'Erase all settings and reboot?',resetting:'Resetting...',rebootSetup:'Device will reboot into setup mode.'},de:{connected:'Verbunden',playing:'Spielt',connecting:'Verbindet',stopped:'Gestoppt',error:'Fehler',selectStation:'Bitte Sender ausw\\u00e4hlen',saved:'Gespeichert',volSaved:'Lautst\\u00e4rke gespeichert',eqSaved:'EQ gespeichert',stationsSaved:'Sender gespeichert',noStations:'keine Sender, Bearbeiten',activeWake:'Aktives Wake Word',rebootNeeded:'\\u00c4nderung gilt nach Neustart',confirmReset:'Alle Einstellungen l\\u00f6schen und neu starten?',resetting:'Zur\\u00fccksetzen...',rebootSetup:'Das Ger\\u00e4t startet im Setup-Modus.'},es:{connected:'Conectado',playing:'Reproduciendo',connecting:'Conectando',stopped:'Detenido',error:'Error',selectStation:'Selecciona una emisora',saved:'Guardado',volSaved:'Volumen guardado',eqSaved:'EQ guardado',stationsSaved:'Emisoras guardadas',noStations:'sin emisoras, Editar',activeWake:'Wake word activo',rebootNeeded:'el cambio se aplica tras reiniciar',confirmReset:'Borrar ajustes y reiniciar?',resetting:'Restableciendo...',rebootSetup:'El dispositivo reiniciar\\u00e1 en modo setup.'},pt:{connected:'Ligado',playing:'A tocar',connecting:'A ligar',stopped:'Parado',error:'Erro',selectStation:'Escolhe uma esta\\u00e7\\u00e3o',saved:'Guardado',volSaved:'Volume guardado',eqSaved:'EQ guardado',stationsSaved:'Esta\\u00e7\\u00f5es guardadas',noStations:'sem esta\\u00e7\\u00f5es, Editar',activeWake:'Wake word ativo',rebootNeeded:'a altera\\u00e7\\u00e3o aplica ap\\u00f3s reiniciar',confirmReset:'Apagar defini\\u00e7\\u00f5es e reiniciar?',resetting:'A repor...',rebootSetup:'O dispositivo reinicia em modo setup.'},it:{connected:'Connesso',playing:'In riproduzione',connecting:'Connessione',stopped:'Fermo',error:'Errore',selectStation:'Seleziona una stazione',saved:'Salvato',volSaved:'Volume salvato',eqSaved:'EQ salvato',stationsSaved:'Stazioni salvate',noStations:'nessuna stazione, Modifica',activeWake:'Wake word attiva',rebootNeeded:'la modifica vale dopo il riavvio',confirmReset:'Cancellare impostazioni e riavviare?',resetting:'Ripristino...',rebootSetup:'Il dispositivo si riavvier\\u00e0 in setup.'},pl:{connected:'Po\\u0142\\u0105czono',playing:'Odtwarzanie',connecting:'\\u0141\\u0105czenie',stopped:'Zatrzymano',error:'B\\u0142\\u0105d',selectStation:'Wybierz stacj\\u0119',saved:'Zapisano',volSaved:'G\\u0142o\\u015bno\\u015b\\u0107 zapisana',eqSaved:'EQ zapisany',stationsSaved:'Stacje zapisane',noStations:'brak stacji, Edytuj',activeWake:'Aktywne wake word',rebootNeeded:'zmiana dzia\\u0142a po restarcie',confirmReset:'Usun\\u0105\\u0107 ustawienia i uruchomi\\u0107 ponownie?',resetting:'Resetowanie...',rebootSetup:'Urz\\u0105dzenie uruchomi si\\u0119 w trybie setup.'}};"
"var infoText={hu:['Az AIC3204 DAC-ban \\u00e9rv\\u00e9nyes\\u00fcl, teh\\u00e1t a r\\u00e1di\\u00f3t \\u00e9s a hangv\\u00e1laszt is \\u00e9rinti.','Illeszd be az OpenAI vagy kompatibilis API kulcsot.','Csak nem OpenAI endpointn\\u00e1l m\\u00f3dos\\u00edtsd.',dict.hu.adminInfo,'Wake ut\\u00e1n ennyi m\\u00e1sodperc helyi besz\\u00e9d n\\u00e9lk\\u00fcl z\\u00e1r a session.','K\\u00f6zvetlen MP3, AAC, M4A, TS \\u00e9s WAV HTTP streamek.','', 'Legfeljebb 8 \\u00e1llom\\u00e1s. \\u00dcres n\\u00e9v \\u00e9s URL t\\u00f6rli a sort.'],en:['Applied in the AIC3204 DAC, so it affects radio and voice playback.','Paste your OpenAI or compatible API key here.','Change only for non-OpenAI endpoints.',dict.en.adminInfo,'After wake, the API session closes after this many seconds without local speech.','Supports direct MP3, AAC, M4A, TS and WAV HTTP streams.','', 'Up to 8 stations. Leave name and URL both empty to remove a row.'],de:['Wirkt im AIC3204 DAC, also auf Radio und Sprachwiedergabe.','OpenAI- oder kompatiblen API-Schl\\u00fcssel einf\\u00fcgen.','Nur f\\u00fcr Nicht-OpenAI-Endpunkte \\u00e4ndern.',dict.de.adminInfo,'Nach Wake schlie\\u00dft die API-Session nach so vielen Sekunden ohne lokale Sprache.','Direkte MP3-, AAC-, M4A-, TS- und WAV-HTTP-Streams.','', 'Bis zu 8 Sender. Name und URL leer lassen, um eine Zeile zu l\\u00f6schen.'],es:['Se aplica en el DAC AIC3204, afecta radio y voz.','Pega tu clave API de OpenAI o compatible.','Cambia solo para endpoints que no sean OpenAI.',dict.es.adminInfo,'Tras wake, la sesi\\u00f3n API se cierra tras estos segundos sin voz local.','Streams HTTP directos MP3, AAC, M4A, TS y WAV.','', 'Hasta 8 emisoras. Deja nombre y URL vac\\u00edos para borrar una fila.'],pt:['Aplicado no DAC AIC3204, afeta r\\u00e1dio e voz.','Cola a chave API OpenAI ou compat\\u00edvel.','Altera apenas para endpoints que n\\u00e3o sejam OpenAI.',dict.pt.adminInfo,'Depois do wake, a sess\\u00e3o API fecha ap\\u00f3s estes segundos sem fala local.','Streams HTTP diretos MP3, AAC, M4A, TS e WAV.','', 'At\\u00e9 8 esta\\u00e7\\u00f5es. Nome e URL vazios removem a linha.'],it:['Applicato nel DAC AIC3204, quindi riguarda radio e voce.','Incolla la chiave API OpenAI o compatibile.','Modifica solo per endpoint non OpenAI.',dict.it.adminInfo,'Dopo il wake, la sessione API si chiude dopo questi secondi senza voce locale.','Stream HTTP diretti MP3, AAC, M4A, TS e WAV.','', 'Fino a 8 stazioni. Lascia nome e URL vuoti per eliminare la riga.'],pl:['Dzia\\u0142a w DAC AIC3204, wi\\u0119c dotyczy radia i g\\u0142osu.','Wklej klucz API OpenAI lub kompatybilny.','Zmieniaj tylko dla endpoint\\u00f3w innych ni\\u017c OpenAI.',dict.pl.adminInfo,'Po wake sesja API zamknie si\\u0119 po tylu sekundach bez lokalnej mowy.','Bezpo\\u015brednie strumienie HTTP MP3, AAC, M4A, TS i WAV.','', 'Maksymalnie 8 stacji. Pusta nazwa i URL usuwa wiersz.']};"
"function t(k){return (dict[lang]&&dict[lang][k])||dict.en[k]||k;}"
"function w(k){return (uiWords[lang]&&uiWords[lang][k])||uiWords.en[k]||k;}"
"function setTxt(id,txt){var e=$(id);if(e)e.textContent=txt;}"
"function applyLanguage(l){lang=dict[l]?l:'hu';document.documentElement.lang=lang;setTxt('ui-lang-label',t('ui'));"
"var hs=document.querySelectorAll('.card h2');if(hs[0])hs[0].innerHTML='&#127926; '+t('vol')+' & EQ';if(hs[1])hs[1].innerHTML='&#128273; '+t('api');if(hs[2])hs[2].innerHTML='&#127908; '+t('voice');if(hs[3])hs[3].innerHTML='&#127911; '+t('media');if(hs[4])hs[4].innerHTML='&#9881; '+t('system');"
"var labels=document.querySelectorAll('.card label');var vals=[t('vol'),t('eq'),t('apikey'),'API Endpoint',t('admin'),t('wake'),sensLabel[lang]||sensLabel.en,t('rtvoice'),t('style'),t('basePrompt'),t('addPrompt'),t('timeout'),t('radio'),'Snapcast',t('dev')];labels.forEach(function(e,i){if(vals[i])e.textContent=vals[i];});"
"var so=sensOpts[lang]||sensOpts.en;var ss=$('ww-sensitivity');if(ss){for(var j=0;j<ss.options.length&&j<so.length;j++)ss.options[j].textContent=so[j];}"
"setTxt('admin-key-info',t('adminInfo'));var b=document.querySelectorAll('button');if(b[0])b[0].textContent=t('saveApi');if(b[1])b[1].textContent=t('saveVoice');if(b[2])b[2].textContent=t('play');if(b[3])b[3].textContent=t('stop');if(b[4])b[4].textContent=t('edit');if(b[7])b[7].textContent=t('saveDev');if(b[8])b[8].textContent=t('reboot')||'Restart device';if(b[9])b[9].textContent=t('reset');"
"var st=t('styles');var opts=$('conv-style').options;for(var i=0;i<opts.length&&i<st.length;i++)opts[i].textContent=st[i];"
"var inf=infoText[lang]||infoText.en;document.querySelectorAll('.info').forEach(function(e,i){if(inf[i])e.textContent=inf[i];});"
"$('apikey').placeholder=t('apikey');$('adminkey').placeholder=t('admin');$('system-prompt').placeholder=t('addPrompt');}"
"function applyStatus(d){"
"$('title').innerHTML='&#9881; '+(d.device_name||'MiciMike');"
"if(document.activeElement!==$('device-name'))$('device-name').value=suffixFromName(d.device_name);"
"$('rssi').textContent=(d.rssi_dbm===null||d.rssi_dbm===undefined)?'RSSI -- dBm':'RSSI '+d.rssi_dbm+' dBm';"
"led('dac-led',!!d.dac_ready);$('dac-text').textContent=d.dac_ready?'DAC OK':'DAC ERR';"
"led('pa-led',!!d.pa_on);$('pa-text').textContent=d.pa_on?'PA ON':'PA OFF';"
"led('mute-led',!d.muted);$('mute-text').textContent=d.muted?'MUTE ON':'MUTE OFF';"
"if($('wakeword-runtime')){$('wakeword-runtime').textContent=w('activeWake')+': '+(d.runtime_wakeword||d.wakeword||'okay_nabu')+(d.wakeword_reboot_required?' ('+w('rebootNeeded')+')':'');}"
"var u=d.usage||{};if(!orgUsageActive){$('tok-text').className='usage '+(u.has_usage?'ok':'');$('tok-text').title=u.raw_json||'';"
"$('tok-text').textContent=u.has_usage?('TOK '+u.total_tokens+' / '+u.input_tokens+' in / '+u.output_tokens+' out'):'TOK --';}"
"}"

// Load current settings
"var radioStations=[];var radioCurrentIdx=-1;"
"fetch('/api/settings').then(r=>r.json()).then(d=>{"
"applyStatus(d);"
"$('apikey').value=d.api_key||'';"
"$('adminkey').value=d.admin_api_key||'';"
"$('apiurl').value=d.api_url||'';"
"$('wakeword').value=d.wakeword||'okay_nabu';"
"$('ww-sensitivity').value=d.wakeword_sensitivity||'moderate';"
"$('rt-voice').value=d.realtime_voice||'marin';"
"$('ui-language').value=d.ui_language||'hu';"
"applyLanguage($('ui-language').value);"
"$('conv-style').value=d.conversation_style||'default';"
"$('system-prompt').value=d.system_prompt||'';"
"var vol=(d.volume===undefined||d.volume===null)?30:d.volume;"
"$('volume').value=vol;"
"$('vol-val').textContent=vol;"
"$('session-timeout').value=d.session_timeout_s||10;"
"$('eq-profile').value=d.eq_profile||'Loudness';"
"$('ip').textContent=w('connected')+' - '+d.ip;"
"var ih=d.free_internal_heap||0, ib=d.largest_internal_block||0, sh=d.free_spiram_heap||0, sb=d.largest_spiram_block||0;"
"var ifrag=ih?Math.round((1-ib/ih)*100):0, sfrag=sh?Math.round((1-sb/sh)*100):0;"
"$('fwinfo').textContent='Firmware v1 | ESP-IDF v6.0.1 | Free heap: '+d.free_heap+' B | internal '+ih+' B largest '+ib+' B frag '+ifrag+'% | PSRAM '+sh+' B largest '+sb+' B frag '+sfrag+'%';"
"radioStations=d.radio_stations||[];radioCurrentIdx=(d.radio_current_index===undefined?-1:d.radio_current_index);"
"renderStationDropdown();"
"refreshOrgUsage();"
"});"

"function renderStationDropdown(){"
"var sel=$('radio-station');sel.innerHTML='';"
"if(!radioStations.length){var o=document.createElement('option');o.value='-1';o.textContent='-- '+w('noStations')+' --';sel.appendChild(o);return;}"
"radioStations.forEach(function(s,i){var o=document.createElement('option');o.value=i;o.textContent=s.name||('Station '+(i+1));sel.appendChild(o);});"
"if(radioCurrentIdx>=0&&radioCurrentIdx<radioStations.length)sel.value=radioCurrentIdx;"
"}"

"function renderStationEditor(){"
"var c=$('station-rows');c.innerHTML='';"
"var rows=radioStations.slice();while(rows.length<8)rows.push({name:'',url:''});"
"rows.forEach(function(s,i){"
"var d=document.createElement('div');d.className='row';d.style.marginBottom='4px';"
"d.innerHTML='<input data-bwignore=\"true\" autocomplete=\"off\" autocorrect=\"off\" autocapitalize=\"off\" spellcheck=\"false\" data-i=\"'+i+'\" data-f=\"name\" placeholder=\"Name\" style=\"max-width:140px\" value=\"'+(s.name||'').replace(/\"/g,'&quot;')+'\">'+"
"'<input data-bwignore=\"true\" autocomplete=\"off\" autocorrect=\"off\" autocapitalize=\"off\" spellcheck=\"false\" data-i=\"'+i+'\" data-f=\"url\" placeholder=\"https://...\" value=\"'+(s.url||'').replace(/\"/g,'&quot;')+'\">';"
"c.appendChild(d);});"
"}"

"function toggleStationEditor(){"
"var box=$('station-editor');"
"if(box.style.display==='none'){renderStationEditor();box.style.display='block';}"
"else box.style.display='none';"
"}"

"function saveStations(){"
"var inputs=$('station-rows').querySelectorAll('input');"
"var buf={};inputs.forEach(function(el){var i=el.dataset.i;buf[i]=buf[i]||{name:'',url:''};buf[i][el.dataset.f]=el.value.trim();});"
"var list=[];Object.keys(buf).sort(function(a,b){return a-b;}).forEach(function(k){var s=buf[k];if(s.name||s.url)list.push({name:s.name||('Station '+(parseInt(k)+1)),url:s.url});});"
"fetch('/api/radio/stations',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({stations:list})"
"}).then(r=>r.json()).then(function(d){"
"radioStations=d.stations||[];radioCurrentIdx=(d.current_index===undefined?-1:d.current_index);"
"renderStationDropdown();show('radio-status',w('stationsSaved')+' ('+radioStations.length+')',1);"
"$('station-editor').style.display='none';"
"}).catch(e=>show('radio-status',w('error')+': '+e,0));"
"}"

"function refreshRadioNow(){"
"fetch('/api/radio/status').then(r=>r.json()).then(d=>{"
"var n=d.state==='playing'?(w('playing')+': '+(d.station_name||d.url||'')):"
"(d.state==='connecting'?(w('connecting')+'...'):(d.state==='error'?(w('error')+': '+(d.error||'')):w('stopped')));"
"$('radio-now').textContent=n;"
"}).catch(()=>{});"
"}"
"setInterval(refreshRadioNow,2500);refreshRadioNow();"
"function refreshStatus(){fetch('/api/settings').then(r=>r.json()).then(applyStatus).catch(()=>{});}"
"setInterval(refreshStatus,3000);"
"function refreshOrgUsage(){fetch('/api/openai/usage').then(r=>r.json()).then(function(d){"
"if(!d.available){orgUsageActive=(d.reason&&d.reason!=='missing_admin_key');if(orgUsageActive){$('tok-text').className='usage';$('tok-text').title=JSON.stringify(d);$('tok-text').textContent='TOK admin '+(d.status||d.reason);}return;}orgUsageActive=true;var fmt=function(n){return n>=1000000?(Math.round(n/100000)/10+'M'):(n>=1000?(Math.round(n/100)/10+'k'):n);};"
"$('tok-text').className='usage ok';$('tok-text').title='Today '+d.today_tokens+' | Week '+d.week_tokens+' | Month '+d.month_tokens;"
"$('tok-text').textContent='TOK D '+fmt(d.today_tokens)+' / W '+fmt(d.week_tokens)+' / M '+fmt(d.month_tokens);"
"}).catch(()=>{});}"
"setInterval(refreshOrgUsage,600000);"

"var volTimer=null;"
"$('volume').oninput=function(){"
"$('vol-val').textContent=this.value;"
"clearTimeout(volTimer);"
"var v=parseInt(this.value);"
"volTimer=setTimeout(function(){saveVolume(v);},700);"
"};"

"function saveApi(){"
"fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({api_key:$('apikey').value,admin_api_key:$('adminkey').value,api_url:$('apiurl').value})"
"}).then(r=>r.ok?show('api-status',w('saved'),1):show('api-status',w('error'),0))"
".then(refreshOrgUsage)"
".catch(e=>show('api-status',w('error')+': '+e,0));}"

"function saveUiLanguage(){applyLanguage($('ui-language').value);"
"fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ui_language:$('ui-language').value})}).then(refreshStatus).catch(()=>{});}"

"function saveDevice(){"
"fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({device_name:$('device-name').value})"
"}).then(r=>r.ok?show('device-status',w('saved'),1):show('device-status',w('error'),0))"
".then(refreshStatus)"
".catch(e=>show('device-status',w('error')+': '+e,0));}"

"function saveVoice(){"
"fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({wakeword:$('wakeword').value,wakeword_sensitivity:$('ww-sensitivity').value,realtime_voice:$('rt-voice').value,ui_language:$('ui-language').value,conversation_style:$('conv-style').value,system_prompt:$('system-prompt').value,session_timeout_s:parseInt($('session-timeout').value)})"
"}).then(r=>r.ok?show('voice-status',w('saved'),1):show('voice-status',w('error'),0))"
".then(refreshStatus)"
".catch(e=>show('voice-status',w('error')+': '+e,0));}"

"function saveVolume(v){"
"fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({volume:v})"
"}).then(r=>r.ok?show('volume-status',w('volSaved'),1):show('volume-status',w('error'),0))"
".catch(e=>show('volume-status',w('error')+': '+e,0));}"

"function saveEq(){"
"fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({eq_profile:$('eq-profile').value})"
"}).then(r=>r.ok?show('eq-status',w('eqSaved'),1):show('eq-status',w('error'),0))"
".catch(e=>show('eq-status',w('error')+': '+e,0));}"

"function playRadio(){"
"var idx=parseInt($('radio-station').value);"
"if(isNaN(idx)||idx<0||idx>=radioStations.length){show('radio-status',w('selectStation'),0);return;}"
"fetch('/api/radio/play',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({index:idx})"
"}).then(r=>r.text().then(t=>show('radio-status',t,r.ok)))"
".then(refreshRadioNow)"
".catch(e=>show('radio-status',w('error')+': '+e,0));}"

"function stopRadio(){"
"fetch('/api/radio/stop',{method:'POST'}).then(r=>r.text().then(t=>show('radio-status',t,r.ok)))"
".then(refreshRadioNow)"
".catch(e=>show('radio-status',w('error')+': '+e,0));}"

"function rebootDevice(){fetch('/api/reboot',{method:'POST'}).finally(()=>{document.body.innerHTML="
"'<div class=wrap><h1>'+w('resetting')+'</h1></div>';"
"setTimeout(function waitBoot(){fetch('/api/settings?ts='+Date.now(),{cache:'no-store'}).then(function(r){if(r.ok)location.href='/';else setTimeout(waitBoot,1500);}).catch(function(){setTimeout(waitBoot,1500);});},5000);});}"

"function factoryReset(){"
"if(confirm(w('confirmReset'))){"
"fetch('/api/reset',{method:'POST'}).then(()=>{document.body.innerHTML="
"'<div class=wrap><h1>'+w('resetting')+'</h1><p>'+w('rebootSetup')+'</p></div>';})}}"

"</script></div></body></html>";

// ---------------------------------------------------------------------------
// GET /api/settings — return current config as JSON
// ---------------------------------------------------------------------------
static esp_err_t api_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    // Mask API key — show only last 4 chars
    if (strlen(live_cfg->api_key) > 4) {
        char masked[sizeof(live_cfg->api_key)];
        int len = strlen(live_cfg->api_key);
        memset(masked, '*', len - 4);
        memcpy(masked + len - 4, live_cfg->api_key + len - 4, 4);
        masked[len] = '\0';
        cJSON_AddStringToObject(root, "api_key", masked);
    } else {
        cJSON_AddStringToObject(root, "api_key", "");
    }

    if (strlen(live_cfg->admin_api_key) > 4) {
        char masked[sizeof(live_cfg->admin_api_key)];
        int len = strlen(live_cfg->admin_api_key);
        memset(masked, '*', len - 4);
        memcpy(masked + len - 4, live_cfg->admin_api_key + len - 4, 4);
        masked[len] = '\0';
        cJSON_AddStringToObject(root, "admin_api_key", masked);
    } else {
        cJSON_AddStringToObject(root, "admin_api_key", "");
    }

    cJSON_AddStringToObject(root, "api_url", live_cfg->api_url);
    cJSON_AddStringToObject(root, "device_name", live_cfg->device_name);
    cJSON_AddStringToObject(root, "radio_url", live_cfg->radio_url);  // legacy, for backward-compat
    cJSON *stations_arr = cJSON_CreateArray();
    for (uint8_t i = 0; i < live_cfg->radio_station_count && i < MAX_RADIO_STATIONS; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "name", live_cfg->radio_stations[i].name);
        cJSON_AddStringToObject(s, "url", live_cfg->radio_stations[i].url);
        cJSON_AddItemToArray(stations_arr, s);
    }
    cJSON_AddItemToObject(root, "radio_stations", stations_arr);
    cJSON_AddNumberToObject(root, "radio_current_index", live_cfg->radio_current_index);
    cJSON_AddStringToObject(root, "eq_profile", live_cfg->eq_profile);
    cJSON_AddStringToObject(root, "wakeword", live_cfg->wakeword);
    cJSON_AddStringToObject(root, "runtime_wakeword", runtime_wakeword);
    cJSON_AddBoolToObject(root, "wakeword_reboot_required", runtime_wakeword_reboot_required);
    cJSON_AddStringToObject(root, "wakeword_sensitivity", live_cfg->wakeword_sensitivity);
    cJSON_AddStringToObject(root, "realtime_voice", live_cfg->realtime_voice);
    cJSON_AddStringToObject(root, "conversation_style", live_cfg->conversation_style);
    cJSON_AddStringToObject(root, "ui_language", live_cfg->ui_language);
    cJSON_AddStringToObject(root, "system_prompt", live_cfg->system_prompt);
    cJSON_AddNumberToObject(root, "volume", live_cfg->volume);
    cJSON_AddNumberToObject(root, "session_timeout_s", live_cfg->session_timeout_s);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "free_internal_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "largest_internal_block", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_spiram_heap", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "largest_spiram_block", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    ws_usage_info_t usage;
    ws_client_get_last_usage(&usage);
    cJSON *usage_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(usage_obj, "has_usage", usage.has_usage);
    cJSON_AddNumberToObject(usage_obj, "response_count", usage.response_count);
    cJSON_AddNumberToObject(usage_obj, "input_tokens", usage.input_tokens);
    cJSON_AddNumberToObject(usage_obj, "output_tokens", usage.output_tokens);
    cJSON_AddNumberToObject(usage_obj, "total_tokens", usage.total_tokens);
    cJSON_AddStringToObject(usage_obj, "raw_json", usage.raw_json);
    cJSON_AddItemToObject(root, "usage", usage_obj);
    cJSON_AddBoolToObject(root, "dac_ready", aic3204_is_ready());
    cJSON_AddBoolToObject(root, "pa_on", gpio_get_level(PIN_SPEAKER_AMP) == 1);
    bool muted = gpio_get_level(PIN_MUTE_SWITCH) == 1;
    cJSON_AddBoolToObject(root, "muted", muted);
    cJSON_AddNumberToObject(root, "mute_gpio", muted ? 1 : 0);
    int8_t rssi = 0;
    if (get_wifi_rssi(&rssi)) {
        cJSON_AddNumberToObject(root, "rssi_dbm", rssi);
    } else {
        cJSON_AddNullToObject(root, "rssi_dbm");
    }

    // Get IP address
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        cJSON_AddStringToObject(root, "ip", ip_str);
    } else {
        cJSON_AddStringToObject(root, "ip", "unknown");
    }

    const char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);

    free((void *)json);
    cJSON_Delete(root);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /api/settings — update config fields
// ---------------------------------------------------------------------------
static esp_err_t api_post_handler(httpd_req_t *req)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, buf, 4095);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    bool changed = false;
    uint32_t changed_mask = 0;

    cJSON *item;
    item = cJSON_GetObjectItem(root, "api_key");
    if (item && cJSON_IsString(item) && strlen(item->valuestring) > 0) {
        // Only update if not all asterisks (masked value sent back)
        if (item->valuestring[0] != '*') {
            strncpy(live_cfg->api_key, item->valuestring, sizeof(live_cfg->api_key) - 1);
            live_cfg->api_key[sizeof(live_cfg->api_key) - 1] = '\0';
            changed = true;
            changed_mask |= SETTINGS_CHANGED_API;
        }
    }

    item = cJSON_GetObjectItem(root, "admin_api_key");
    if (item && cJSON_IsString(item) && strlen(item->valuestring) > 0) {
        if (item->valuestring[0] != '*') {
            strncpy(live_cfg->admin_api_key, item->valuestring, sizeof(live_cfg->admin_api_key) - 1);
            live_cfg->admin_api_key[sizeof(live_cfg->admin_api_key) - 1] = '\0';
            changed = true;
            changed_mask |= SETTINGS_CHANGED_API;
        }
    }

    item = cJSON_GetObjectItem(root, "api_url");
    if (item && cJSON_IsString(item) && strlen(item->valuestring) > 0) {
        strncpy(live_cfg->api_url, item->valuestring, sizeof(live_cfg->api_url) - 1);
        live_cfg->api_url[sizeof(live_cfg->api_url) - 1] = '\0';
        changed = true;
        changed_mask |= SETTINGS_CHANGED_API;
    }

    item = cJSON_GetObjectItem(root, "device_name");
    if (item && cJSON_IsString(item)) {
        strncpy(live_cfg->device_name, item->valuestring, sizeof(live_cfg->device_name) - 1);
        live_cfg->device_name[sizeof(live_cfg->device_name) - 1] = '\0';
        normalize_device_name(live_cfg->device_name, sizeof(live_cfg->device_name));
        apply_hostname(live_cfg->device_name);
        changed = true;
    }

    item = cJSON_GetObjectItem(root, "radio_url");
    if (item && cJSON_IsString(item)) {
        strncpy(live_cfg->radio_url, item->valuestring, sizeof(live_cfg->radio_url) - 1);
        live_cfg->radio_url[sizeof(live_cfg->radio_url) - 1] = '\0';
        changed = true;
    }

    item = cJSON_GetObjectItem(root, "eq_profile");
    if (item && cJSON_IsString(item)) {
        esp_err_t eq_ret = aic3204_set_eq_profile(item->valuestring);
        if (eq_ret == ESP_OK) {
            strncpy(live_cfg->eq_profile, item->valuestring, sizeof(live_cfg->eq_profile) - 1);
            live_cfg->eq_profile[sizeof(live_cfg->eq_profile) - 1] = '\0';
            changed = true;
        } else {
            ESP_LOGW(TAG, "Failed to apply EQ profile '%s': %s",
                     item->valuestring, esp_err_to_name(eq_ret));
        }
    }

    item = cJSON_GetObjectItem(root, "wakeword");
    if (item && cJSON_IsString(item)) {
        if (strcmp(live_cfg->wakeword, item->valuestring) != 0) {
            strncpy(live_cfg->wakeword, item->valuestring, sizeof(live_cfg->wakeword) - 1);
            live_cfg->wakeword[sizeof(live_cfg->wakeword) - 1] = '\0';
            changed = true;
            changed_mask |= SETTINGS_CHANGED_WAKEWORD;
        }
    }

    item = cJSON_GetObjectItem(root, "wakeword_sensitivity");
    if (item && cJSON_IsString(item) && is_valid_wakeword_sensitivity(item->valuestring)) {
        if (strcmp(live_cfg->wakeword_sensitivity, item->valuestring) != 0) {
            strncpy(live_cfg->wakeword_sensitivity, item->valuestring, sizeof(live_cfg->wakeword_sensitivity) - 1);
            live_cfg->wakeword_sensitivity[sizeof(live_cfg->wakeword_sensitivity) - 1] = '\0';
            changed = true;
            changed_mask |= SETTINGS_CHANGED_WW_SENS;
        }
    }

    item = cJSON_GetObjectItem(root, "realtime_voice");
    if (item && cJSON_IsString(item) && is_valid_realtime_voice(item->valuestring)) {
        strncpy(live_cfg->realtime_voice, item->valuestring, sizeof(live_cfg->realtime_voice) - 1);
        live_cfg->realtime_voice[sizeof(live_cfg->realtime_voice) - 1] = '\0';
        changed = true;
        changed_mask |= SETTINGS_CHANGED_VOICE;
    }

    item = cJSON_GetObjectItem(root, "conversation_style");
    if (item && cJSON_IsString(item) && is_valid_conversation_style(item->valuestring)) {
        strncpy(live_cfg->conversation_style, item->valuestring, sizeof(live_cfg->conversation_style) - 1);
        live_cfg->conversation_style[sizeof(live_cfg->conversation_style) - 1] = '\0';
        changed = true;
        changed_mask |= SETTINGS_CHANGED_STYLE;
    }

    item = cJSON_GetObjectItem(root, "ui_language");
    if (item && cJSON_IsString(item) && is_valid_ui_language(item->valuestring)) {
        strncpy(live_cfg->ui_language, item->valuestring, sizeof(live_cfg->ui_language) - 1);
        live_cfg->ui_language[sizeof(live_cfg->ui_language) - 1] = '\0';
        changed = true;
    }

    item = cJSON_GetObjectItem(root, "system_prompt");
    if (item && cJSON_IsString(item)) {
        strncpy(live_cfg->system_prompt, item->valuestring, sizeof(live_cfg->system_prompt) - 1);
        live_cfg->system_prompt[sizeof(live_cfg->system_prompt) - 1] = '\0';
        changed = true;
        changed_mask |= SETTINGS_CHANGED_PROMPT;
    }

    item = cJSON_GetObjectItem(root, "volume");
    if (item && cJSON_IsNumber(item)) {
        int volume = item->valueint;
        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;
        live_cfg->volume = (uint8_t)volume;
        aic3204_set_volume(live_cfg->volume);
        led_control_show_volume(live_cfg->volume);
        changed = true;
    }

    item = cJSON_GetObjectItem(root, "session_timeout_s");
    if (item && cJSON_IsNumber(item)) {
        int timeout_s = item->valueint;
        if (timeout_s < 3) timeout_s = 3;
        if (timeout_s > 300) timeout_s = 300;
        live_cfg->session_timeout_s = (uint16_t)timeout_s;
        changed = true;
        changed_mask |= SETTINGS_CHANGED_TIMEOUT;
    }

    if (changed) {
        nvs_config_save(live_cfg);
        if (settings_changed_cb) {
            settings_changed_cb(changed_mask);
        }
        ESP_LOGI(TAG, "Settings updated and saved: mask=0x%08lx", (unsigned long)changed_mask);
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /api/reset — factory reset
// ---------------------------------------------------------------------------
static esp_err_t api_reset_handler(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "Resetting...");
    ESP_LOGW(TAG, "Factory reset requested!");
    vTaskDelay(pdMS_TO_TICKS(1000));
    nvs_config_clear();
    esp_restart();
    return ESP_OK;
}

static uint64_t json_num_u64(cJSON *obj, const char *name)
{
    cJSON *item = cJSON_GetObjectItem(obj, name);
    return (item && cJSON_IsNumber(item)) ? (uint64_t)item->valuedouble : 0;
}

static esp_err_t api_openai_usage_handler(httpd_req_t *req)
{
    cJSON *out = cJSON_CreateObject();
    if (strlen(live_cfg->admin_api_key) < 8 || live_cfg->admin_api_key[0] == '*') {
        cJSON_AddBoolToObject(out, "available", false);
        cJSON_AddStringToObject(out, "reason", "missing_admin_key");
        char *json = cJSON_PrintUnformatted(out);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json);
        free(json);
        cJSON_Delete(out);
        return ESP_OK;
    }

    time_t now = time(NULL);
    if (now < 1700000000) {
        cJSON_AddBoolToObject(out, "available", false);
        cJSON_AddStringToObject(out, "reason", "clock_not_synced");
        char *json = cJSON_PrintUnformatted(out);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json);
        free(json);
        cJSON_Delete(out);
        return ESP_OK;
    }

    time_t month_start = now - (time_t)(31 * 24 * 60 * 60);
    time_t week_start = now - (time_t)(7 * 24 * 60 * 60);
    time_t day_start = now - (time_t)(24 * 60 * 60);
    char url[192];
    snprintf(url, sizeof(url),
             "https://api.openai.com/v1/organization/usage/completions?start_time=%lld&bucket_width=1d&limit=31",
             (long long)month_start);

    char auth[300];
    snprintf(auth, sizeof(auth), "Bearer %s", live_cfg->admin_api_key);

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 12000,
        .buffer_size = 2048,
        .user_agent = "micimike-ai-fw/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        cJSON_AddBoolToObject(out, "available", false);
        cJSON_AddStringToObject(out, "reason", "http_init_failed");
        goto respond;
    }
    esp_http_client_set_header(client, "Authorization", auth);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        cJSON_AddBoolToObject(out, "available", false);
        cJSON_AddStringToObject(out, "reason", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        goto respond;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    const int usage_body_cap = 65536;
    char *body = heap_caps_malloc(usage_body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        cJSON_AddBoolToObject(out, "available", false);
        cJSON_AddStringToObject(out, "reason", "oom");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto respond;
    }

    int total = 0;
    while (total < usage_body_cap - 1) {
        int r = esp_http_client_read(client, body + total, usage_body_cap - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    body[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total >= usage_body_cap - 1) {
        cJSON_AddBoolToObject(out, "available", false);
        cJSON_AddStringToObject(out, "reason", "response_truncated");
        cJSON_AddNumberToObject(out, "bytes", total);
        free(body);
        goto respond;
    }

    if (status < 200 || status >= 300) {
        cJSON_AddBoolToObject(out, "available", false);
        cJSON_AddNumberToObject(out, "status", status);
        cJSON_AddStringToObject(out, "reason", "openai_error");
        cJSON *err_root = cJSON_Parse(body);
        cJSON *err_obj = err_root ? cJSON_GetObjectItem(err_root, "error") : NULL;
        cJSON *err_msg = err_obj ? cJSON_GetObjectItem(err_obj, "message") : NULL;
        if (err_msg && cJSON_IsString(err_msg)) {
            cJSON_AddStringToObject(out, "message", err_msg->valuestring);
        }
        if (err_root) {
            cJSON_Delete(err_root);
        }
        ESP_LOGW(TAG, "OpenAI usage request failed: status=%d", status);
        free(body);
        goto respond;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        cJSON_AddBoolToObject(out, "available", false);
        cJSON_AddStringToObject(out, "reason", "parse_failed");
        ESP_LOGW(TAG, "OpenAI usage parse failed: status=%d bytes=%d", status, total);
        goto respond;
    }

    uint64_t day_tokens = 0;
    uint64_t week_tokens = 0;
    uint64_t month_tokens = 0;
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *bucket = NULL;
    cJSON_ArrayForEach(bucket, data) {
        time_t start_time = (time_t)json_num_u64(bucket, "start_time");
        cJSON *results = cJSON_GetObjectItem(bucket, "results");
        cJSON *result = NULL;
        uint64_t bucket_tokens = 0;
        cJSON_ArrayForEach(result, results) {
            bucket_tokens += json_num_u64(result, "input_tokens");
            bucket_tokens += json_num_u64(result, "output_tokens");
            bucket_tokens += json_num_u64(result, "input_audio_tokens");
            bucket_tokens += json_num_u64(result, "output_audio_tokens");
        }
        if (start_time >= month_start) month_tokens += bucket_tokens;
        if (start_time >= week_start) week_tokens += bucket_tokens;
        if (start_time >= day_start) day_tokens += bucket_tokens;
    }
    cJSON_Delete(root);

    cJSON_AddBoolToObject(out, "available", true);
    cJSON_AddNumberToObject(out, "today_tokens", (double)day_tokens);
    cJSON_AddNumberToObject(out, "week_tokens", (double)week_tokens);
    cJSON_AddNumberToObject(out, "month_tokens", (double)month_tokens);

respond:
    {
        char *json = cJSON_PrintUnformatted(out);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json);
        free(json);
        cJSON_Delete(out);
    }
    return ESP_OK;
}

static const char *radio_state_name(radio_state_t state)
{
    switch (state) {
    case RADIO_STATE_STOPPED: return "stopped";
    case RADIO_STATE_CONNECTING: return "connecting";
    case RADIO_STATE_PLAYING: return "playing";
    case RADIO_STATE_ERROR: return "error";
    default: return "unknown";
    }
}

static esp_err_t api_radio_play_handler(httpd_req_t *req)
{
    char buf[384];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Accept either {"index": N} (preferred) or {"url": "..."} (legacy).
    const char *play_url = NULL;
    cJSON *idx_item = cJSON_GetObjectItem(root, "index");
    if (idx_item && cJSON_IsNumber(idx_item)) {
        int idx = idx_item->valueint;
        if (idx >= 0 && idx < (int)live_cfg->radio_station_count) {
            play_url = live_cfg->radio_stations[idx].url;
            live_cfg->radio_current_index = (int8_t)idx;
        }
    }
    if (!play_url) {
        cJSON *url = cJSON_GetObjectItem(root, "url");
        if (url && cJSON_IsString(url) && url->valuestring[0]) {
            play_url = url->valuestring;
            strncpy(live_cfg->radio_url, url->valuestring, sizeof(live_cfg->radio_url) - 1);
            live_cfg->radio_url[sizeof(live_cfg->radio_url) - 1] = '\0';
        }
    }
    if (!play_url || play_url[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Station index or URL required");
        return ESP_FAIL;
    }

    // Persist the selected station so it survives reboots.
    nvs_config_save(live_cfg);

    media_radio_clear_pending();  // explicit play overrides any pending resume
    esp_err_t ret = media_radio_start(play_url);
    cJSON_Delete(root);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, media_radio_get_error());
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "Radio starting");
    return ESP_OK;
}

static esp_err_t api_radio_stop_handler(httpd_req_t *req)
{
    media_radio_stop();
    media_radio_clear_pending();  // explicit user stop — don't auto-resume later
    httpd_resp_sendstr(req, "Radio stopped");
    return ESP_OK;
}

static esp_err_t api_radio_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", radio_state_name(media_radio_get_state()));
    cJSON_AddStringToObject(root, "error", media_radio_get_error());

    int idx = live_cfg->radio_current_index;
    if (idx >= 0 && idx < (int)live_cfg->radio_station_count) {
        cJSON_AddStringToObject(root, "station_name", live_cfg->radio_stations[idx].name);
        cJSON_AddStringToObject(root, "url", live_cfg->radio_stations[idx].url);
    } else {
        cJSON_AddStringToObject(root, "station_name", "");
        cJSON_AddStringToObject(root, "url", live_cfg->radio_url);
    }
    cJSON_AddNumberToObject(root, "current_index", idx);

    const char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);

    free((void *)json);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/radio/stations — replace the station list
// GET  /api/radio/stations — return the station list + current_index
static esp_err_t api_radio_stations_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (uint8_t i = 0; i < live_cfg->radio_station_count && i < MAX_RADIO_STATIONS; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "name", live_cfg->radio_stations[i].name);
        cJSON_AddStringToObject(s, "url",  live_cfg->radio_stations[i].url);
        cJSON_AddItemToArray(arr, s);
    }
    cJSON_AddItemToObject(root, "stations", arr);
    cJSON_AddNumberToObject(root, "current_index", live_cfg->radio_current_index);

    const char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free((void *)json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_radio_stations_post_handler(httpd_req_t *req)
{
    // Up to 8 stations * (~32 name + ~224 url + JSON overhead) ≈ 2.5 KiB
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, buf, 4095);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "stations");
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing stations[]");
        return ESP_FAIL;
    }

    // Snapshot the previously-selected station's URL so we can re-locate it
    // in the new list by URL (index-based selection becomes stale after edit).
    char prev_sel_url[sizeof(live_cfg->radio_stations[0].url)] = {0};
    if (live_cfg->radio_current_index >= 0
        && live_cfg->radio_current_index < (int)live_cfg->radio_station_count) {
        strncpy(prev_sel_url,
                live_cfg->radio_stations[live_cfg->radio_current_index].url,
                sizeof(prev_sel_url) - 1);
    }

    uint8_t count = 0;
    int array_size = cJSON_GetArraySize(arr);
    for (int i = 0; i < array_size && count < MAX_RADIO_STATIONS; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(item)) continue;
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *url  = cJSON_GetObjectItem(item, "url");
        if (!url || !cJSON_IsString(url) || url->valuestring[0] == '\0') continue;

        memset(&live_cfg->radio_stations[count], 0, sizeof(radio_station_t));
        if (name && cJSON_IsString(name) && name->valuestring[0]) {
            strncpy(live_cfg->radio_stations[count].name, name->valuestring,
                    sizeof(live_cfg->radio_stations[count].name) - 1);
        } else {
            snprintf(live_cfg->radio_stations[count].name,
                     sizeof(live_cfg->radio_stations[count].name),
                     "Station %u", (unsigned)(count + 1));
        }
        strncpy(live_cfg->radio_stations[count].url, url->valuestring,
                sizeof(live_cfg->radio_stations[count].url) - 1);
        count++;
    }
    // Zero out the leftover slots so a future blob read doesn't see stale data.
    for (uint8_t i = count; i < MAX_RADIO_STATIONS; i++) {
        memset(&live_cfg->radio_stations[i], 0, sizeof(radio_station_t));
    }
    live_cfg->radio_station_count = count;

    // Re-resolve previously-selected station by URL match; -1 if it's gone.
    int8_t new_idx = -1;
    if (prev_sel_url[0]) {
        for (uint8_t i = 0; i < count; i++) {
            if (strcmp(live_cfg->radio_stations[i].url, prev_sel_url) == 0) {
                new_idx = (int8_t)i;
                break;
            }
        }
    }
    live_cfg->radio_current_index = new_idx;

    nvs_config_save(live_cfg);
    cJSON_Delete(root);

    // Reply with the canonical state so the client can render it back.
    return api_radio_stations_get_handler(req);
}

// ---------------------------------------------------------------------------
// GET / — serve settings page
// ---------------------------------------------------------------------------
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, SETTINGS_HTML, sizeof(SETTINGS_HTML) - 1);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
esp_err_t settings_server_start(micimike_config_t *cfg, settings_changed_cb_t changed_cb)
{
    live_cfg = cfg;
    settings_changed_cb = changed_cb;

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 14;   // root, settings, reset/reboot, usage, radio play/stop/status/stations
    http_cfg.stack_size = 8192;

    esp_err_t ret = httpd_start(&server, &http_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start settings server: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t api_get = { .uri = "/api/settings", .method = HTTP_GET, .handler = api_get_handler };
    httpd_uri_t api_post = { .uri = "/api/settings", .method = HTTP_POST, .handler = api_post_handler };
    httpd_uri_t api_reset = { .uri = "/api/reset", .method = HTTP_POST, .handler = api_reset_handler };
    httpd_uri_t api_reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = api_reboot_handler };
    httpd_uri_t api_usage = { .uri = "/api/openai/usage", .method = HTTP_GET, .handler = api_openai_usage_handler };
    httpd_uri_t api_radio_play = { .uri = "/api/radio/play", .method = HTTP_POST, .handler = api_radio_play_handler };
    httpd_uri_t api_radio_stop = { .uri = "/api/radio/stop", .method = HTTP_POST, .handler = api_radio_stop_handler };
    httpd_uri_t api_radio_status = { .uri = "/api/radio/status", .method = HTTP_GET, .handler = api_radio_status_handler };
    httpd_uri_t api_radio_stations_get = { .uri = "/api/radio/stations", .method = HTTP_GET,  .handler = api_radio_stations_get_handler };
    httpd_uri_t api_radio_stations_post = { .uri = "/api/radio/stations", .method = HTTP_POST, .handler = api_radio_stations_post_handler };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &api_get);
    httpd_register_uri_handler(server, &api_post);
    httpd_register_uri_handler(server, &api_reset);
    httpd_register_uri_handler(server, &api_reboot);
    httpd_register_uri_handler(server, &api_usage);
    httpd_register_uri_handler(server, &api_radio_play);
    httpd_register_uri_handler(server, &api_radio_stop);
    httpd_register_uri_handler(server, &api_radio_status);
    httpd_register_uri_handler(server, &api_radio_stations_get);
    httpd_register_uri_handler(server, &api_radio_stations_post);

    // Log the URL
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "Settings UI: http://" IPSTR "/", IP2STR(&ip_info.ip));
    }

    return ESP_OK;
}

esp_err_t settings_server_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    return ESP_OK;
}

static void delayed_reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t api_reboot_handler(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "Restarting...");
    ESP_LOGW(TAG, "Restart requested from Web UI");
    xTaskCreate(delayed_reboot_task, "web_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}
