#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <Preferences.h>

#include <TFT_eSPI.h>
#include <lvgl.h>

#include <XPT2046_Touchscreen.h>

#include <WiFiManager.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>

// ----------------- Display -----------------
static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 320;
static constexpr int DISPLAY_ROTATION = 2; // 180°

static TFT_eSPI tft;

// ----------------- Touch (XPT2046) -----------------
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

static SPIClass spiTouch(HSPI);
static XPT2046_Touchscreen touch(XPT2046_CS, XPT2046_IRQ);

// ----------------- LVGL buffers -----------------
static lv_disp_draw_buf_t drawBuf;
static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;

// ----------------- UI -----------------
static lv_obj_t *labelHeader;
static lv_obj_t *labelStatus;
static lv_obj_t *listPorts;

// ----------------- Preferences -----------------
static Preferences prefs;

// ----------------- MQTT -----------------
static AsyncMqttClient mqtt;
static bool mqttConfigured = false;

// Configurable (loaded from NVS / WiFiManager)
static String cfg_mqtt_host;
static uint16_t cfg_mqtt_port = 1883;
static String cfg_mqtt_user;
static String cfg_mqtt_pass;
static String cfg_topic_filter; // supports wildcard

// ----------------- WiFiManager -----------------
static WiFiManager wm;
static bool shouldSaveConfig = false;

// WiFiManager custom parameters (buffers must outlive wm)
static char p_mqtt_host[64] = "";
static char p_mqtt_port[8] = "1883";
static char p_mqtt_user[64] = "";
static char p_mqtt_pass[64] = "";
static char p_topic[128] = "windows/+/serialports/state";

// ----------------- Touch calibration defaults -----------------
static int CAL_X_MIN = 200, CAL_X_MAX = 3800;
static int CAL_Y_MIN = 200, CAL_Y_MAX = 3800;

// ----------------- LVGL callbacks -----------------
static void lv_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
	const int32_t w = (area->x2 - area->x1 + 1);
	const int32_t h = (area->y2 - area->y1 + 1);

	tft.startWrite();
	tft.setAddrWindow(area->x1, area->y1, w, h);
	tft.pushColors((uint16_t *) &color_p->full, w * h, true);
	tft.endWrite();

	lv_disp_flush_ready(disp);
}

static void ui_set_status(const String &text) {
	lv_label_set_text(labelStatus, text.c_str());
}

static void ui_clear_ports() {
	lv_obj_clean(listPorts);
}

static void ui_add_port(const char *port, const char *name) {
	String line = String(port) + "  " + (name ? name : "");
	lv_list_add_btn(listPorts, LV_SYMBOL_USB, line.c_str());
}

static void ui_init() {
	lv_obj_t *scr = lv_scr_act();
	lv_obj_set_style_pad_all(scr, 8, 0);

	labelHeader = lv_label_create(scr);
	lv_label_set_text(labelHeader, "Serial Ports");
	lv_obj_set_style_text_font(labelHeader, &lv_font_montserrat_20, 0);
	lv_obj_align(labelHeader, LV_ALIGN_TOP_LEFT, 0, 0);

	labelStatus = lv_label_create(scr);
	lv_label_set_text(labelStatus, "Booting...");
	lv_obj_align(labelStatus, LV_ALIGN_TOP_RIGHT, 0, 4);

	listPorts = lv_list_create(scr);
	lv_obj_set_size(listPorts, lv_pct(100), lv_pct(85));
	lv_obj_align(listPorts, LV_ALIGN_BOTTOM_MID, 0, 0);
}

// Touch -> LVGL pointer
static void lv_touch_read_cb(lv_indev_drv_t *indev, lv_indev_data_t *data) {
	(void) indev;

	if (!touch.touched()) {
		data->state = LV_INDEV_STATE_REL;
		return;
	}

	TS_Point p = touch.getPoint();

	int x = map(p.x, CAL_X_MIN, CAL_X_MAX, 0, SCREEN_W - 1);
	int y = map(p.y, CAL_Y_MIN, CAL_Y_MAX, 0, SCREEN_H - 1);

	if (DISPLAY_ROTATION == 2) {
		x = (SCREEN_W - 1) - x;
		y = (SCREEN_H - 1) - y;
	}

	x = constrain(x, 0, SCREEN_W - 1);
	y = constrain(y, 0, SCREEN_H - 1);

	data->point.x = x;
	data->point.y = y;
	data->state = LV_INDEV_STATE_PR;
}

static void init_lvgl_display() {
	tft.init();
	tft.setRotation(DISPLAY_ROTATION);
	tft.fillScreen(TFT_BLACK);

	lv_init();

	const size_t bufLines = 40;
	buf1 = (lv_color_t *) heap_caps_malloc(SCREEN_W * bufLines * sizeof(lv_color_t), MALLOC_CAP_DMA);
	buf2 = (lv_color_t *) heap_caps_malloc(SCREEN_W * bufLines * sizeof(lv_color_t), MALLOC_CAP_DMA);
	lv_disp_draw_buf_init(&drawBuf, buf1, buf2, SCREEN_W * bufLines);

	static lv_disp_drv_t disp_drv;
	lv_disp_drv_init(&disp_drv);
	disp_drv.hor_res = SCREEN_W;
	disp_drv.ver_res = SCREEN_H;
	disp_drv.flush_cb = lv_flush_cb;
	disp_drv.draw_buf = &drawBuf;
	lv_disp_drv_register(&disp_drv);

	static lv_indev_drv_t indev_drv;
	lv_indev_drv_init(&indev_drv);
	indev_drv.type = LV_INDEV_TYPE_POINTER;
	indev_drv.read_cb = lv_touch_read_cb;
	lv_indev_drv_register(&indev_drv);

	ui_init();
}

// ----------------- Touch init -----------------
static void init_touch() {
	spiTouch.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
	touch.begin(spiTouch);
	touch.setRotation(DISPLAY_ROTATION);
}

// ----------------- Config persistence -----------------
static void load_config() {
	prefs.begin("cydcfg", true);

	cfg_mqtt_host = prefs.getString("mh", "");
	cfg_mqtt_port = prefs.getUShort("mp", 1883);
	cfg_mqtt_user = prefs.getString("mu", "");
	cfg_mqtt_pass = prefs.getString("ms", "");
	cfg_topic_filter = prefs.getString("tf", "windows/+/serialports/state");

	// Also load calibration if you decide to store it later
	// CAL_X_MIN = prefs.getInt("cxn", CAL_X_MIN); ...

	prefs.end();

	mqttConfigured = (cfg_mqtt_host.length() > 0 && cfg_topic_filter.length() > 0);

	// Fill WiFiManager param buffers with current values (so portal shows them)
	strlcpy(p_mqtt_host, cfg_mqtt_host.c_str(), sizeof(p_mqtt_host));
	snprintf(p_mqtt_port, sizeof(p_mqtt_port), "%u", (unsigned) cfg_mqtt_port);
	strlcpy(p_mqtt_user, cfg_mqtt_user.c_str(), sizeof(p_mqtt_user));
	strlcpy(p_mqtt_pass, cfg_mqtt_pass.c_str(), sizeof(p_mqtt_pass));
	strlcpy(p_topic, cfg_topic_filter.c_str(), sizeof(p_topic));
}

static void save_config_from_params() {
	cfg_mqtt_host = String(p_mqtt_host);
	cfg_mqtt_port = (uint16_t) atoi(p_mqtt_port);
	cfg_mqtt_user = String(p_mqtt_user);
	cfg_mqtt_pass = String(p_mqtt_pass);
	cfg_topic_filter = String(p_topic);

	prefs.begin("cydcfg", false);
	prefs.putString("mh", cfg_mqtt_host);
	prefs.putUShort("mp", cfg_mqtt_port);
	prefs.putString("mu", cfg_mqtt_user);
	prefs.putString("ms", cfg_mqtt_pass);
	prefs.putString("tf", cfg_topic_filter);
	prefs.end();

	mqttConfigured = (cfg_mqtt_host.length() > 0 && cfg_topic_filter.length() > 0);
}

// WiFiManager callback when “Save” hit
static void on_save_config() {
	shouldSaveConfig = true;
}

// ----------------- MQTT handling -----------------
static void mqtt_subscribe() {
	if (!mqttConfigured) return;
	mqtt.subscribe(cfg_topic_filter.c_str(), 1);
}

static void mqtt_connect() {
	if (!mqttConfigured) {
		ui_set_status("MQTT: not configured");
		return;
	}

	mqtt.setServer(cfg_mqtt_host.c_str(), cfg_mqtt_port);

	if (cfg_mqtt_user.length() > 0) {
		mqtt.setCredentials(cfg_mqtt_user.c_str(), cfg_mqtt_pass.c_str());
	}

	mqtt.connect();
}

static void handle_state_json(const char *payload, size_t len) {
	JsonDocument doc; // dynamic document
	doc.reserve(4096);
	DeserializationError err = deserializeJson(doc, payload, len);
	if (err) {
		ui_set_status("JSON error");
		return;
	}

	const char *host = doc["host"] | "PC";
	JsonVariant portsVar = doc["ports"];
	if (!portsVar.is<JsonArray>()) {
		// payload didn't match expected schema
		lv_label_set_text(labelHeader, host);
		ui_clear_ports();
		ui_set_status("JSON: no ports[]");
		return;
	}
	JsonArray ports = portsVar.as<JsonArray>();

	String header = String(host) + " (" + String(ports.size()) + ")";
	lv_label_set_text(labelHeader, header.c_str());

	ui_clear_ports();
	for (JsonObject p: ports) {
		const char *port = p["port"] | "?";
		const char *name = p["name"] | "";
		ui_add_port(port, name);
	}
}

static void on_mqtt_connect(bool sessionPresent) {
	(void) sessionPresent;
	ui_set_status("MQTT: connected");
	mqtt_subscribe();
}

static void on_mqtt_disconnect(AsyncMqttClientDisconnectReason reason) {
	(void) reason;
	ui_set_status("MQTT: disconnected");
}

static void on_mqtt_message(char *topic, char *payload,
							AsyncMqttClientMessageProperties properties,
							size_t len, size_t index, size_t total) {
	(void) properties;
	(void) index;
	(void) total;
	// We only subscribe to one filter, so treat any message as “state”
	handle_state_json(payload, len);
}

// ----------------- WiFi provisioning + portal -----------------
static String make_ap_name() {
	uint32_t chip = (uint32_t) ESP.getEfuseMac();
	char ap[32];
	snprintf(ap, sizeof(ap), "CYD-SerialPorts-%04X", (unsigned) (chip & 0xFFFF));
	return String(ap);
}

static void start_portal_and_connect() {
	// Add custom fields into portal
	WiFiManagerParameter prm_mh("mh", "MQTT host/IP", p_mqtt_host, sizeof(p_mqtt_host));
	WiFiManagerParameter prm_mp("mp", "MQTT port", p_mqtt_port, sizeof(p_mqtt_port));
	WiFiManagerParameter prm_mu("mu", "MQTT user (optional)", p_mqtt_user, sizeof(p_mqtt_user));
	WiFiManagerParameter prm_ms("ms", "MQTT pass (optional)", p_mqtt_pass, sizeof(p_mqtt_pass));
	WiFiManagerParameter prm_tf("tf", "Topic filter (wildcards ok)", p_topic, sizeof(p_topic));

	wm.setSaveConfigCallback(on_save_config);

	wm.addParameter(&prm_mh);
	wm.addParameter(&prm_mp);
	wm.addParameter(&prm_mu);
	wm.addParameter(&prm_ms);
	wm.addParameter(&prm_tf);

	// Non-blocking portal so LVGL keeps running
	wm.setConfigPortalBlocking(false);

	// Keep portal available even after joining WiFi:
	// - start config portal if needed
	// - then start web portal on STA
	const String apName = make_ap_name();
	ui_set_status("WiFi: starting portal");
	wm.setConfigPortalTimeout(0); // 0 = no timeout (you can set e.g. 180)

	// Attempt auto-connect; if no creds, it opens AP portal
	bool connected = wm.autoConnect(apName.c_str());

	if (connected) {
		ui_set_status("WiFi: connected");
		// Start portal on STA so you can edit later at http://device-ip/
		wm.startWebPortal();
	} else {
		// In non-blocking mode, autoConnect may return false while portal runs.
		ui_set_status("WiFi: portal active");
	}

	// Copy any updated parameter values from WiFiManager objects into buffers:
	// (WiFiManagerParameter stores updated values in its internal buffer which
	// is the same p_* we passed in, so p_* now contains user input.)
	if (shouldSaveConfig) {
		save_config_from_params();
		shouldSaveConfig = false;
	}
}

void setup() {
	Serial.begin(115200);

	init_lvgl_display();
	init_touch();

	// Load persisted config into buffers so portal shows current values
	load_config();

	// MQTT callbacks
	mqtt.onConnect(on_mqtt_connect);
	mqtt.onDisconnect(on_mqtt_disconnect);
	mqtt.onMessage(on_mqtt_message);

	// Start provisioning + (eventually) WiFi
	start_portal_and_connect();
}

void loop() {
	// Keep LVGL responsive
	lv_timer_handler();

	// Keep WiFiManager portal responsive (captive portal + STA portal)
	wm.process();

	// Once connected and MQTT not connected, connect
	static bool mqttStarted = false;
	if (WiFi.isConnected() && !mqttStarted) {
		mqttStarted = true;

		IPAddress ip = WiFi.localIP();
		ui_set_status(String("IP: ") + ip.toString());

		// If user saved config during portal, it’s already persisted.
		// Now connect to MQTT using saved settings.
		mqtt_connect();

		// Helpful: show where to configure later
		// (WiFiManager portal will be at http://<ip>/ )
		delay(250);
		ui_set_status(String("Cfg: http://") + ip.toString());
	}

	delay(5);
}
