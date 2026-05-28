#include "esp_camera.h"
#include <WiFi.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "fb_gfx.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_http_server.h"
//libraries


// ===========================
// WiFi Settings
// ===========================
const char *ssid = "STEMRouter";
const char *password = "robot1234";


// ===========================
// Motor Pins
// ===========================
// define pins
#define IN1 14  // Left Motor Forward
#define IN2 15  // Left Motor Backward
#define IN3 13  // Right Motor Forward
#define IN4 12  // Right Motor Backward
#define IN5 2   //dribbler
#define IN6 1   //kicker
//to initialize and obtain ip address, comment out ln6 stuff

// ===========================
// AI THINKER ESP-32 pins
// ===========================
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// ===========================
// Stream setup
// ===========================
#define PART_BOUNDARY "123456789000000000000987654321"                                         //unique boundary between different frame data sets to separate them
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;  //replace old photo with new one when you see the divider like a flipbook
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";                            //put the divider in
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";     //"the data following this will be a jpeg image"


httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;  //creating pointers( kind of like place markers) for two servers (one for camera and one for controls)


// ===========================
// Motor control functions
// ===========================
void stopMotors() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}
void moveForward() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}
void moveBackward() {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}
void turnLeft() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}
void turnRight() {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}
void dribble() {
  digitalWrite(IN5, LOW);
  digitalWrite(IN6, HIGH);
}
void kick() {
  digitalWrite(IN5, HIGH);
  digitalWrite(IN6, LOW);  //comment out! for ip address
}
void sdk() {
  digitalWrite(IN5, LOW);
  digitalWrite(IN6, LOW);  //comment out! for ip address
}
// ===========================
// Webpage
// ===========================
//progmem -> use flash memory not RAM to save memory and prevent crashes
//rawliteral-> "treat everything in here as pure text"
//picking what the webpage looks like using CSS (cascading style sheets language)
//div class controls -> creates webpage buttons and formats them(?): fwd, back, dribble, etc
//data-cmd -> “labelling” the webpage buttons with their functions
//fetch frame -> basically prints a bunch of pictures like a flipbook to make feed, making sure that it updates asap and doesn’t just chill on an old photo
//keep an eye on specific keyboard keys because they are attached to functions too
//document.querySelectorAll('.btn').forEach(btn =>... -> runs the command attached to the button when it is pressed and stops the command when the button is not pressed
//keyMap -> binds keys to commands: w for forward, s for backwards, etc
//const held = new Set();... -> holding down a key will make the command run constantly for as long as the key is held down
static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 Rover</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { background: #1a1a1a; color: white; font-family: monospace; display: flex; flex-direction: column; align-items: center; min-height: 100vh; }
    h1 { padding: 16px; color: #00ff88; font-size: 1.2em; }
    canvas { max-width: 100%; border: 2px solid #00ff88; margin: 8px; }
    #status { color: #aaa; font-size: 0.8em; margin-bottom: 8px; }
    .controls { display: grid; grid-template-columns: repeat(3, 80px); grid-template-rows: repeat(3, 80px); gap: 8px; margin: 16px; }
    .btn {
      background: #2a2a2a; border: 2px solid #00ff88; border-radius: 12px;
      color: #00ff88; font-size: 1.8em; cursor: pointer;
      display: flex; align-items: center; justify-content: center;
      user-select: none; -webkit-user-select: none;
      transition: background 0.1s;
    }
    .btn:active, .btn.pressed { background: #00ff88; color: #1a1a1a; }
    .blank { visibility: hidden; }
  </style>
</head>
<body>
  <h1>ESP32 Rover Control</h1>
  <p id="status">Connecting...</p>
  <canvas id="canvas"></canvas>

  <div class="controls">
    <div class="btn" id="btn-dribble"  data-cmd="dribble">q</div>
    <div class="btn" id="btn-forward"  data-cmd="forward">&#9650;</div>
    <div class="btn" id="btn-kick"     data-cmd="kick">e</div>
    <div class="btn" id="btn-left"     data-cmd="left">&#9664;</div>
    <div class="btn" id="btn-stop"     data-cmd="stop">&#9632;</div>
    <div class="btn" id="btn-right"    data-cmd="right">&#9654;</div>
    <div class="blank"></div>
    <div class="btn" id="btn-backward" data-cmd="backward">&#9660;</div>
    <div class="btn" id="btn-sdk"     data-cmd="sdk">c</div>
  </div>

  <script>
    const canvas = document.getElementById('canvas');
    const ctx = canvas.getContext('2d');
    const status = document.getElementById('status');
    let fetching = false;

    function fetchFrame() {
      if (fetching) return;
      fetching = true;
      const img = new Image();
      img.onload = function() {
        canvas.width = img.width;
        canvas.height = img.height;
        ctx.drawImage(img, 0, 0);
        URL.revokeObjectURL(img.src);
        status.textContent = 'Live';
        fetching = false;
        requestAnimationFrame(fetchFrame);
      };
      img.onerror = function() {
        URL.revokeObjectURL(img.src);
        fetching = false;
        status.textContent = 'Reconnecting...';
        setTimeout(fetchFrame, 500);
      };
      fetch('/snapshot?t=' + Date.now())
        .then(r => r.blob())
        .then(blob => { img.src = URL.createObjectURL(blob); })
        .catch(() => { fetching = false; status.textContent = 'Reconnecting...'; setTimeout(fetchFrame, 500); });
    }

    fetchFrame();

    function sendCmd(cmd) {
      fetch('/motor?cmd=' + cmd).catch(() => {});
    }


    document.querySelectorAll('.btn').forEach(btn => {
      const cmd = btn.dataset.cmd;
      btn.addEventListener('mousedown',  () => { btn.classList.add('pressed');    sendCmd(cmd); });
      btn.addEventListener('mouseup',    () => { btn.classList.remove('pressed'); if (cmd !== 'stop') sendCmd('stop'); });
      btn.addEventListener('mouseleave', () => { btn.classList.remove('pressed'); if (cmd !== 'stop') sendCmd('stop'); });
      btn.addEventListener('touchstart', (e) => { e.preventDefault(); btn.classList.add('pressed');    sendCmd(cmd); });
      btn.addEventListener('touchend',   (e) => { e.preventDefault(); btn.classList.remove('pressed'); if (cmd !== 'stop') sendCmd('stop'); });
    });


    const keyMap = {
      'ArrowUp': 'forward', 'ArrowDown': 'backward',
      'ArrowLeft': 'left', 'ArrowRight': 'right',
      'w': 'forward', 'W': 'forward',
      's': 'backward', 'S': 'backward',
      'a': 'left', 'A': 'left',
      'd': 'right', 'D': 'right',
      'q': 'dribble', 'Q': 'dribble',
      'e': 'kick', 'E': 'kick',
      'c': 'sdk', 'C': 'sdk',
    };


    const held = new Set();
    document.addEventListener('keydown', (e) => {
      if (keyMap[e.key] && !held.has(e.key)) {
        held.add(e.key);
        const btnEl = document.getElementById('btn-' + keyMap[e.key]);
        if (btnEl) btnEl.classList.add('pressed');
        sendCmd(keyMap[e.key]);
      }
    });
    document.addEventListener('keyup', (e) => {
      if (keyMap[e.key]) {
        held.delete(e.key);
        const btnEl = document.getElementById('btn-' + keyMap[e.key]);
        if (btnEl) btnEl.classList.remove('pressed');
        sendCmd('stop');
      }
    });
  </script>
</body>
</html>
)rawliteral";


// ===========================
// Snapshot handler
// ===========================
static esp_err_t snapshot_handler(httpd_req_t *req) {  //take picture function
  camera_fb_t *fb = esp_camera_fb_get();
  esp_camera_fb_return(fb);
  fb = esp_camera_fb_get();  //get a photo, discard it, and get a new photo
                             //fb-> frame buffer, pointer variable


  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;  //error
  }


  httpd_resp_set_type(req, "image/jpeg");  //there will be a jpeg image
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");  //webpage should download a new photo each time (cache-busting)


  esp_err_t res;
  if (fb->format == PIXFORMAT_JPEG) {
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);  //if it’s formatted, send it over wifi
  } else {                                                       //if it’s not a jpeg, convert it to a jpeg
    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    bool converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
    if (converted) {
      res = httpd_resp_send(req, (const char *)jpg_buf, jpg_len);
      free(jpg_buf);
    } else {
      httpd_resp_send_500(req);
      res = ESP_FAIL;
    }
  }

  esp_camera_fb_return(fb);
  return res;  //get rid of old photo
}


// ===========================
// Motor handler
// ===========================
static esp_err_t motor_handler(httpd_req_t *req) {
  char query[64];
  char cmd[16];

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  //makes the commands/button pressing actually do stuff; the “forward” command is bound to the moveForward function, etc
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (httpd_query_key_value(query, "cmd", cmd, sizeof(cmd)) == ESP_OK) {
      if (strcmp(cmd, "forward") == 0) moveForward();
      else if (strcmp(cmd, "backward") == 0) moveBackward();
      else if (strcmp(cmd, "left") == 0) turnLeft();
      else if (strcmp(cmd, "right") == 0) turnRight();
      else if (strcmp(cmd, "stop") == 0) stopMotors();
      else if (strcmp(cmd, "dribble") == 0) dribble();
      else if (strcmp(cmd, "kick") == 0) kick();
      else if (strcmp(cmd, "sdk") == 0) sdk();
    }
  }

  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

// ===========================
// Stream handler (VLC)
// ===========================
//rapidly sends photos in jpeg format over wifi to create a live stream
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char *part_buf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  
  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
    } else {
      if (fb->format != PIXFORMAT_JPEG) {
        bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!jpeg_converted) res = ESP_FAIL;
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;
      }
    }


    if (res == ESP_OK) {
      size_t hlen = snprintf((char *)part_buf, 64, STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);  //sends the boundary string of numbers, what the size of the image data will be and then the image data
    }


    if (fb) {
      esp_camera_fb_return(fb);  //you can rewrite over the old photo!
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if (res != ESP_OK) break;
  }
  return res;
}


// ===========================
// Index handler
// ===========================
//runs index_html to create the webpage designed there at the ip address
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}

// ===========================
// Start servers
// ===========================
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;


  httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
  httpd_uri_t snap_uri = { .uri = "/snapshot", .method = HTTP_GET, .handler = snapshot_handler, .user_ctx = NULL };
  httpd_uri_t motor_uri = { .uri = "/motor", .method = HTTP_GET, .handler = motor_handler, .user_ctx = NULL };  // setting the different functions to run on the webpage


  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &snap_uri);
    httpd_register_uri_handler(camera_httpd, &motor_uri);
    Serial.println("Main server started on port 80");
  }
  //start motor controls server on port 80


  config.server_port = 81;
  config.ctrl_port = 32769;
  httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };  //start stream on port 81


  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    Serial.println("Stream server started on port 81 (VLC)");
  }
}


// ===========================
// Setup
// ===========================
void setup() {
  //Serial monitor
  Serial.begin(115200);
  Serial.setDebugOutput(false);


  // Motor pins
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  pinMode(IN5, OUTPUT);
  pinMode(IN6, OUTPUT);  //comment out! for ip address


  //stop motors on startup
  sdk();
  stopMotors();


  // Camera config, defining which tiny camera wires control what
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  
  if (psramFound()) {  //if extra memory is on this board, use it
    config.frame_size = FRAMESIZE_QQVGA;
    config.jpeg_quality = 15;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    Serial.println("PSRAM found, using CIF");
  } else {
    config.frame_size = FRAMESIZE_QQVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_LATEST;
    Serial.println("No PSRAM, using QVGA");
  }

  //initialize camera
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return;
  }

  //adjust image orientation
  sensor_t *s = esp_camera_sensor_get();
  s->set_hmirror(s, 1);
  s->set_vflip(s, 0);

  //connect to router & print ip address (and other things but theyre not as important)
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  IPAddress IP = WiFi.localIP();
  Serial.println("WiFi connected!");
  Serial.print("Browser:      http://");
  Serial.println(IP);
  Serial.print("VLC:          http://");
  Serial.print(IP);
  Serial.println(":81/stream");

  startCameraServer();
}


// ===========================
// Loop
// ===========================
void loop() {
}
