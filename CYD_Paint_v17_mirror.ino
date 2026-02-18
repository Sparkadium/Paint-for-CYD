// CYD Paint v17 — ESP32-2432S028 (ILI9341 240x320, XPT2046, SD)
// Added: Mirror mode (MX / MY toggle buttons in strip gap y=200..219)
// TFT via TFT_eSPI (configure User_Setup.h externally)
// Touch on HSPI (CLK=25, MISO=39, MOSI=32, CS=33, IRQ=36)
// SD on VSPI (CLK=18, MISO=19, MOSI=23, CS=5) — default VSPI pins
// Canvas 200x200 heap-allocated, placed at x=20, y=0

#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include <XPT2046_Touchscreen.h>
#include "esp_heap_caps.h"

// ── Pin Definitions ─────────────────────────────────────────────────────────
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_CLK  25
#define TOUCH_MISO 39
#define TOUCH_MOSI 32
#define SD_CS       5
#define BL_PIN     21

// ── Display / Canvas Constants ───────────────────────────────────────────────
#define SCREEN_W   240
#define SCREEN_H   320
#define CANVAS_W   200
#define CANVAS_H   200
#define CANVAS_X    20   // left swatch strip = 0..19, canvas = 20..219, right = 220..239
#define CANVAS_Y    20
#define BOTTOM_Y   220   // bottom bar starts here (120 px tall)
#define ROW_H       30   // each bottom row height

// ── Colour Palette (RGB565) ──────────────────────────────────────────────────
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_BLUE    0x001F
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_MAGENTA 0xF81F
#define C_ORANGE  0xFD20
#define C_PINK    0xFC18
#define C_PURPLE  0x780F
#define C_GRAY    0x7BEF
#define C_DKGRAY  0x39E7
#define C_BROWN   0x8200
#define C_LIME    0x87E0
#define C_NAVY    0x000F

// 12 preset colours
static const uint16_t PRESET_COLORS[12] = {
    C_BLACK, C_WHITE, C_RED, C_GREEN, C_BLUE, C_YELLOW,
    C_CYAN,  C_MAGENTA, C_ORANGE, C_PURPLE, C_BROWN, C_GRAY
};

// ── Tools ────────────────────────────────────────────────────────────────────
enum Tool {
    TOOL_PEN = 0,
    TOOL_ERASER,
    TOOL_FILL,
    TOOL_SPRAY,
    TOOL_LINE,
    TOOL_RECT,
    TOOL_ELLIPSE,
    TOOL_EYEDROP,
    TOOL_COUNT   // sentinel — keep last
};

// Continuous-draw tools: bypass debounce, interpolate strokes
inline bool isContinuousTool(Tool t) {
    return t == TOOL_PEN || t == TOOL_ERASER || t == TOOL_SPRAY;
}
// Two-tap tools: first tap = origin, second tap = commit
inline bool isTwoTapTool(Tool t) {
    return t == TOOL_LINE || t == TOOL_RECT || t == TOOL_ELLIPSE;
}
// Single-tap canvas tools with their own debounce (not UI debounce)
inline bool isSingleTapTool(Tool t) {
    return t == TOOL_FILL || t == TOOL_EYEDROP;
}

// ── Screens ──────────────────────────────────────────────────────────────────
enum Screen { SCR_MAIN = 0, SCR_COLOR_PICK, SCR_GALLERY, SCR_CONFIRM };

// ── Globals ──────────────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

uint16_t *canvas = nullptr;          // 200*200 * 2 bytes
uint16_t customColors[12];           // right strip custom slots
uint16_t activeColor  = C_BLACK;
uint8_t  brushSize    = 3;
Tool     activeTool   = TOOL_PEN;
Screen   currentScreen = SCR_MAIN;
bool     sdOK         = false;
int      fileCount    = 0;

// Confirm screen state
enum ConfirmAction { CA_NEW, CA_CLR, CA_DEL_FILE };
ConfirmAction confirmAction;
int    confirmFileIdx = -1;     // for delete

// Gallery state
int galleryPage = 0;
char galleryFiles[32][13];      // up to 32 filenames, 8.3 + null
int  galleryCount = 0;

// Color picker state
int cpSlot = -1;                // which custom slot we're editing
uint8_t cpR = 255, cpG = 0, cpB = 0;

// Touch debounce (UI elements only — drawing bypasses this)
unsigned long lastTouch = 0;
unsigned long lastCanvasTap = 0;   // separate debounce for Fill/Eyedropper/Shape taps
#define TOUCH_DEBOUNCE_MS    80
#define CANVAS_DEBOUNCE_MS   120   // slightly longer — prevents double-fill on tap

// Smooth drawing state
int  lastDrawX = -1;   // previous canvas-space X (-1 = no previous point)
int  lastDrawY = -1;
bool wasDrawing = false;  // was the stylus on the canvas last frame?

// Mirror mode state
bool mirrorX = false;   // mirror horizontally (flip across vertical centre line)
bool mirrorY = false;   // mirror vertically   (flip across horizontal centre line)

// Two-tap shape state
bool shapeHasOrigin = false;  // waiting for second tap?
int  shapeOriginX   = 0;
int  shapeOriginY   = 0;

// Shared static row buffer for BMP I/O (max 200 pixels × 3 bytes = 600)
static uint8_t rowBuf[CANVAS_W * 3];

// Forward declarations
void drawMainScreen();
void drawBottomBar();
void drawCanvas();
void drawLeftStrip();
void drawRightStrip();
void handleMainTouch(int tx, int ty);
void handleColorPickTouch(int tx, int ty);
void handleGalleryTouch(int tx, int ty);
void handleConfirmTouch(int tx, int ty);
void drawColorPickScreen();
void drawGalleryScreen();
void drawConfirmScreen(const char *msg);
void toolApply(int cx, int cy);
void toolApplyLine(int x0, int y0, int x1, int y1);
void stampBrushAt(int cx, int cy, uint16_t color,
                  int &dirtyX1, int &dirtyY1, int &dirtyX2, int &dirtyY2);
void floodFill(int x, int y, uint16_t newColor);
void spray(int cx, int cy);
void drawShapeLine(int x0, int y0, int x1, int y1, uint16_t color);
void drawShapeRect(int x0, int y0, int x1, int y1, uint16_t color);
void drawShapeEllipse(int cx, int cy, int rx, int ry, uint16_t color);
void pushRegion(int x1, int y1, int x2, int y2);
void saveFile();
void loadFile(int idx);
void scanFiles();
void countFiles();
uint16_t rgb888to565(uint8_t r, uint8_t g, uint8_t b);
void rgb565to888(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b);
void drawSwatchStrip(int x, const uint16_t *colors, int count);
void drawButtonRect(int x, int y, int w, int h, uint16_t bg, uint16_t fg, const char *label);
bool rectHit(int tx, int ty, int x, int y, int w, int h);
uint16_t canvasGet(int x, int y);
void canvasSet(int x, int y, uint16_t c);
void pushCanvas();
void clearCanvas(uint16_t color);

// ─────────────────────────────────────────────────────────────────────────────
// setup / loop
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    // Backlight on
    pinMode(BL_PIN, OUTPUT);
    digitalWrite(BL_PIN, HIGH);

    // TFT
    tft.begin();
    tft.setRotation(0);  // portrait
    tft.setSwapBytes(true);
    tft.fillScreen(TFT_BLACK);

    // Touch HSPI
    touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    touch.begin(touchSPI);
    touch.setRotation(0);

    // SD (VSPI default pins 18/19/23/5)
    sdOK = SD.begin(SD_CS);
    if (!sdOK) Serial.println("SD failed");

    // Allocate canvas
    size_t canvasSz = CANVAS_W * CANVAS_H * sizeof(uint16_t);
    canvas = (uint16_t *)heap_caps_malloc(canvasSz, MALLOC_CAP_8BIT);
    if (!canvas) {
        canvas = (uint16_t *)malloc(canvasSz);
    }
    if (!canvas) {
        tft.drawString("OUT OF MEMORY", 10, 150, 2);
        while (1) delay(1000);
    }
    clearCanvas(C_WHITE);

    // Init custom colors
    for (int i = 0; i < 12; i++) customColors[i] = C_WHITE;

    countFiles();
    drawMainScreen();
}

void loop() {
    bool touching = touch.tirqTouched() && touch.touched();

    if (!touching) {
        if (wasDrawing) {
            lastDrawX  = -1;
            lastDrawY  = -1;
            wasDrawing = false;
        }
        return;
    }

    TS_Point p = touch.getPoint();
    int tx = map(p.x, 200, 3800, 0, 240);
    int ty = map(p.y, 200, 3800, 0, 320);
    tx = constrain(tx, 0, 239);
    ty = constrain(ty, 0, 319);

    // ── Smooth drawing path (no debounce) ────────────────────────────────────
    // Continuous-draw tools (Pen/Eraser/Spray) poll at full hardware rate and
    // interpolate between samples for smooth strokes.
    if (currentScreen == SCR_MAIN &&
        rectHit(tx, ty, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H) &&
        isContinuousTool(activeTool))
    {
        int cx = tx - CANVAS_X;
        int cy = ty - CANVAS_Y;

        if (lastDrawX < 0) {
            toolApply(cx, cy);
        } else {
            toolApplyLine(lastDrawX, lastDrawY, cx, cy);
        }
        lastDrawX  = cx;
        lastDrawY  = cy;
        wasDrawing = true;
        return;
    }

    // ── Single-tap canvas tools (Fill / Eyedropper) ──────────────────────────
    // These need their own debounce, completely independent of the UI debounce,
    // so a tool-select tap doesn't block the very next canvas tap.
    if (currentScreen == SCR_MAIN &&
        rectHit(tx, ty, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H) &&
        isSingleTapTool(activeTool))
    {
        lastDrawX  = -1;
        lastDrawY  = -1;
        wasDrawing = false;

        unsigned long now = millis();
        if (now - lastCanvasTap < CANVAS_DEBOUNCE_MS) return;
        lastCanvasTap = now;

        int cx = tx - CANVAS_X;
        int cy = ty - CANVAS_Y;

        if (activeTool == TOOL_FILL) {
            floodFill(cx, cy, activeColor);
            pushCanvas();
        } else if (activeTool == TOOL_EYEDROP) {
            activeColor = canvasGet(cx, cy);
            drawBottomBar();
        }
        return;
    }

    // ── Two-tap shape tools (also use canvas debounce) ───────────────────────
    if (currentScreen == SCR_MAIN &&
        rectHit(tx, ty, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H) &&
        isTwoTapTool(activeTool))
    {
        lastDrawX  = -1;
        lastDrawY  = -1;
        wasDrawing = false;

        unsigned long now2 = millis();
        if (now2 - lastCanvasTap < CANVAS_DEBOUNCE_MS) return;
        lastCanvasTap = now2;

        // Delegate to handleMainTouch which has the two-tap state machine
        handleMainTouch(tx, ty);
        return;
    }

    // ── UI / one-shot touch path (debounced) ─────────────────────────────────
    lastDrawX  = -1;
    lastDrawY  = -1;
    wasDrawing = false;

    unsigned long nowUI = millis();
    if (nowUI - lastTouch < TOUCH_DEBOUNCE_MS) return;
    lastTouch = nowUI;

    switch (currentScreen) {
        case SCR_MAIN:       handleMainTouch(tx, ty); break;
        case SCR_COLOR_PICK: handleColorPickTouch(tx, ty); break;
        case SCR_GALLERY:    handleGalleryTouch(tx, ty); break;
        case SCR_CONFIRM:    handleConfirmTouch(tx, ty); break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Canvas helpers
// ─────────────────────────────────────────────────────────────────────────────
inline uint16_t canvasGet(int x, int y) {
    return canvas[y * CANVAS_W + x];
}
inline void canvasSet(int x, int y, uint16_t c) {
    canvas[y * CANVAS_W + x] = c;
}
void clearCanvas(uint16_t color) {
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) canvas[i] = color;
}
void pushCanvas() {
    // Push full canvas to TFT at CANVAS_X, CANVAS_Y
    tft.startWrite();
    tft.setAddrWindow(CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H);
    tft.pushPixels(canvas, CANVAS_W * CANVAS_H);
    tft.endWrite();
}

// ─────────────────────────────────────────────────────────────────────────────
// Drawing utilities
// ─────────────────────────────────────────────────────────────────────────────
uint16_t rgb888to565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
void rgb565to888(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
    r = (c >> 8) & 0xF8;
    g = (c >> 3) & 0xFC;
    b = (c << 3) & 0xF8;
}

bool rectHit(int tx, int ty, int x, int y, int w, int h) {
    return tx >= x && tx < x + w && ty >= y && ty < y + h;
}

void drawButtonRect(int x, int y, int w, int h, uint16_t bg, uint16_t fg, const char *label) {
    tft.fillRect(x, y, w, h, bg);
    tft.drawRect(x, y, w, h, fg);
    tft.setTextColor(fg, bg);
    tft.setTextSize(1);
    int tw = strlen(label) * 6;
    int th = 8;
    tft.setCursor(x + (w - tw) / 2, y + (h - th) / 2);
    tft.print(label);
}

void drawSwatchStrip(int x, const uint16_t *colors, int count) {
    int sh = CANVAS_H / count;  // swatch height
    for (int i = 0; i < count; i++) {
        int sy = i * sh;
        tft.fillRect(x, sy, 20, sh, colors[i]);
        tft.drawRect(x, sy, 20, sh, C_DKGRAY);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main screen draw
// ─────────────────────────────────────────────────────────────────────────────
void drawMainScreen() {
    tft.fillScreen(C_DKGRAY);
    drawLeftStrip();
    drawRightStrip();
    pushCanvas();
    drawBottomBar();
    currentScreen = SCR_MAIN;
}

void drawLeftStrip() {
    drawSwatchStrip(0, PRESET_COLORS, 12);
    // Mirror-X toggle button in the gap between swatches and bottom bar (y=200..219)
    uint16_t mxBg = mirrorX ? C_CYAN : 0x4208;
    uint16_t mxFg = mirrorX ? C_BLACK : C_WHITE;
    drawButtonRect(0, 200, 20, 20, mxBg, mxFg, "MX");
}

void drawRightStrip() {
    // Right 20px strip x=220..239
    drawSwatchStrip(220, customColors, 12);
    // Mirror-Y toggle button in the gap between swatches and bottom bar (y=200..219)
    uint16_t myBg = mirrorY ? C_MAGENTA : 0x4208;
    uint16_t myFg = mirrorY ? C_BLACK : C_WHITE;
    drawButtonRect(220, 200, 20, 20, myBg, myFg, "MY");
    tft.setTextColor(C_DKGRAY, C_DKGRAY);
}

void drawCanvas() {
    pushCanvas();
}

void drawBottomBar() {
    // Row 0: y=200..229 — tools + color swatch + brush preview
    // Row 1: y=230..259 — size + colour pick
    // Row 2: y=260..289 — file ops
    // Row 3: y=290..319 — status

    int y0 = BOTTOM_Y;
    tft.fillRect(0, y0, SCREEN_W, 120, 0x2104); // dark bg

    // ── Row 0 ──
    int ry = y0;
    // Tool navigator: [-] prev  [○] reset-to-pen  [+] next
    drawButtonRect(0,  ry, 36, 30, 0x4208, C_WHITE, "<");
    drawButtonRect(36, ry, 36, 30, 0x4208, C_CYAN,  "PEN");
    drawButtonRect(72, ry, 36, 30, 0x4208, C_WHITE, ">");
    // Tool name label (highlighted box)
    const char *toolNames[TOOL_COUNT] = {"Pen","Eraser","Fill","Spray","Line","Rect","Ellipse","Eyedrop"};
    tft.fillRect(108, ry, 52, 30, C_ORANGE);
    tft.drawRect(108, ry, 52, 30, C_WHITE);
    tft.setTextColor(C_BLACK, C_ORANGE);
    tft.setTextSize(1);
    const char *tn = toolNames[activeTool];
    int tw = strlen(tn) * 6;
    tft.setCursor(108 + (52 - tw) / 2, ry + 11);
    tft.print(tn);
    // Active color swatch (40px wide at x=160)
    tft.fillRect(160, ry, 40, 30, activeColor);
    tft.drawRect(160, ry, 40, 30, C_WHITE);
    // Brush preview circle (x=200..239)
    tft.fillRect(200, ry, 40, 30, 0x2104);
    tft.fillCircle(220, ry + 15, min((int)brushSize, 14), activeColor);
    tft.drawCircle(220, ry + 15, min((int)brushSize, 14), C_WHITE);

    // ── Row 1 ──
    ry = y0 + 30;
    drawButtonRect(0,  ry, 30, 30, 0x4208, C_WHITE, "-Sz");
    drawButtonRect(30, ry, 30, 30, 0x4208, C_WHITE, "+Sz");
    // Size number
    tft.fillRect(60, ry, 20, 30, 0x2104);
    tft.setTextColor(C_WHITE, 0x2104);
    tft.setTextSize(1);
    tft.setCursor(63, ry + 11);
    tft.print(brushSize);
    drawButtonRect(80, ry, 160, 30, 0x4208, C_YELLOW, "COLOUR PICK");

    // ── Row 2 ──
    ry = y0 + 60;
    drawButtonRect(0,   ry, 60, 30, 0x4208, C_WHITE, "SAVE");
    drawButtonRect(60,  ry, 60, 30, 0x4208, C_WHITE, "LOAD");
    drawButtonRect(120, ry, 60, 30, 0x4208, C_ORANGE, "NEW");
    drawButtonRect(180, ry, 60, 30, 0x4208, C_RED,    "CLR");
}

// ─────────────────────────────────────────────────────────────────────────────
// Tool application
// ─────────────────────────────────────────────────────────────────────────────

// stampBrushAt — write pixels into canvas[] for one brush circle (or one spray
// burst) at (cx,cy). Does NOT push to the TFT; caller handles that.
// Returns the axis-aligned bounding box that was dirtied via out-params so the
// caller can do a single tight push rather than one per stamp.
void stampBrushAt(int cx, int cy, uint16_t color,
                  int &dirtyX1, int &dirtyY1, int &dirtyX2, int &dirtyY2) {
    if (activeTool == TOOL_SPRAY) {
        int r = brushSize * 4;
        int dots = brushSize * 5;
        for (int i = 0; i < dots; i++) {
            int dx = random(-r, r + 1);
            int dy = random(-r, r + 1);
            if (dx * dx + dy * dy <= r * r) {
                int nx = cx + dx, ny = cy + dy;
                if (nx >= 0 && nx < CANVAS_W && ny >= 0 && ny < CANVAS_H) {
                    canvasSet(nx, ny, color);
                }
            }
        }
        dirtyX1 = max(0, cx - r);
        dirtyY1 = max(0, cy - r);
        dirtyX2 = min(CANVAS_W - 1, cx + r);
        dirtyY2 = min(CANVAS_H - 1, cy + r);
    } else {
        // Pen / Eraser — filled circle
        int r = max(0, (int)brushSize - 1);
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (dx * dx + dy * dy <= r * r) {
                    int nx = cx + dx, ny = cy + dy;
                    if (nx >= 0 && nx < CANVAS_W && ny >= 0 && ny < CANVAS_H) {
                        canvasSet(nx, ny, color);
                    }
                }
            }
        }
        dirtyX1 = max(0, cx - r);
        dirtyY1 = max(0, cy - r);
        dirtyX2 = min(CANVAS_W - 1, cx + r);
        dirtyY2 = min(CANVAS_H - 1, cy + r);
    }
}

// ── Mirror helper ──────────────────────────────────────────────────────────
// Stamp the brush at (cx,cy) PLUS any mirror copies demanded by mirrorX/mirrorY.
// Expands the caller's dirty rect for a single TFT push.
void stampMirrored(int cx, int cy, uint16_t color,
                   int &dX1, int &dY1, int &dX2, int &dY2) {
    int mx = CANVAS_W - 1 - cx;   // mirror x
    int my = CANVAS_H - 1 - cy;   // mirror y

    // Helper lambda-ish macro: stamp and expand dirty rect
    #define STAMP_AND_EXPAND(X,Y) { \
        int bx1,by1,bx2,by2; \
        stampBrushAt((X),(Y),color,bx1,by1,bx2,by2); \
        if(bx1<dX1)dX1=bx1; if(by1<dY1)dY1=by1; \
        if(bx2>dX2)dX2=bx2; if(by2>dY2)dY2=by2; \
    }

    STAMP_AND_EXPAND(cx, cy)
    if (mirrorX)                    STAMP_AND_EXPAND(mx, cy)
    if (mirrorY)                    STAMP_AND_EXPAND(cx, my)
    if (mirrorX && mirrorY)         STAMP_AND_EXPAND(mx, my)
    #undef STAMP_AND_EXPAND
}

// toolApply — single-point application (used for Fill, and as the first sample
// of a new stroke before we have a previous point to interpolate from).
void toolApply(int cx, int cy) {
    if (cx < 0 || cx >= CANVAS_W || cy < 0 || cy >= CANVAS_H) return;

    if (activeTool == TOOL_FILL) {
        floodFill(cx, cy, activeColor);
        pushCanvas();
        return;
    }

    uint16_t drawColor = (activeTool == TOOL_ERASER) ? C_WHITE : activeColor;
    int x1 = CANVAS_W, y1 = CANVAS_H, x2 = 0, y2 = 0;
    stampMirrored(cx, cy, drawColor, x1, y1, x2, y2);

    // Push the combined dirty rectangle
    if (x2 >= x1 && y2 >= y1) {
        int pw = x2 - x1 + 1;
        tft.startWrite();
        for (int row = y1; row <= y2; row++) {
            tft.setAddrWindow(CANVAS_X + x1, CANVAS_Y + row, pw, 1);
            tft.pushPixels(&canvas[row * CANVAS_W + x1], pw);
        }
        tft.endWrite();
    }
}

// toolApplyLine — Bresenham walk from (x0,y0) to (x1,y1), stamping the brush
// at every step, then pushes the entire bounding box in one TFT transaction.
// This is what makes strokes look continuous when the touch controller gives
// us spaced-out samples.
void toolApplyLine(int x0, int y0, int x1, int y1) {
    uint16_t drawColor = (activeTool == TOOL_ERASER) ? C_WHITE : activeColor;

    // Bounding box of everything dirtied — start maximally inverted
    int dX1 = CANVAS_W, dY1 = CANVAS_H, dX2 = 0, dY2 = 0;

    // Helper: Bresenham walk from (ax0,ay0) to (ax1,ay1), stamping brush,
    // expanding the shared dirty rect.
    auto walkLine = [&](int ax0, int ay0, int ax1, int ay1) {
        int dx  =  abs(ax1 - ax0), sx = (ax0 < ax1) ? 1 : -1;
        int dy  = -abs(ay1 - ay0), sy = (ay0 < ay1) ? 1 : -1;
        int err = dx + dy;
        int cx  = ax0, cy = ay0;
        while (true) {
            int scx = constrain(cx, 0, CANVAS_W - 1);
            int scy = constrain(cy, 0, CANVAS_H - 1);
            int bx1, by1, bx2, by2;
            stampBrushAt(scx, scy, drawColor, bx1, by1, bx2, by2);
            if (bx1 < dX1) dX1 = bx1;
            if (by1 < dY1) dY1 = by1;
            if (bx2 > dX2) dX2 = bx2;
            if (by2 > dY2) dY2 = by2;
            if (cx == ax1 && cy == ay1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; cx += sx; }
            if (e2 <= dx) { err += dx; cy += sy; }
        }
    };

    walkLine(x0, y0, x1, y1);
    if (mirrorX) walkLine(CANVAS_W-1-x0, y0, CANVAS_W-1-x1, y1);
    if (mirrorY) walkLine(x0, CANVAS_H-1-y0, x1, CANVAS_H-1-y1);
    if (mirrorX && mirrorY) walkLine(CANVAS_W-1-x0, CANVAS_H-1-y0, CANVAS_W-1-x1, CANVAS_H-1-y1);

    // Single TFT push covering the whole stroke segment
    if (dX2 >= dX1 && dY2 >= dY1) {
        int pw = dX2 - dX1 + 1;
        tft.startWrite();
        for (int row = dY1; row <= dY2; row++) {
            tft.setAddrWindow(CANVAS_X + dX1, CANVAS_Y + row, pw, 1);
            tft.pushPixels(&canvas[row * CANVAS_W + dX1], pw);
        }
        tft.endWrite();
    }
}

void spray(int cx, int cy) {
    // Legacy stub — stampBrushAt handles spray logic now; kept for any
    // direct callers that may be added in future.
    int r = brushSize * 4;
    int dots = brushSize * 5;
    for (int i = 0; i < dots; i++) {
        int dx = random(-r, r + 1);
        int dy = random(-r, r + 1);
        if (dx * dx + dy * dy <= r * r) {
            int nx = cx + dx, ny = cy + dy;
            if (nx >= 0 && nx < CANVAS_W && ny >= 0 && ny < CANVAS_H) {
                canvasSet(nx, ny, activeColor);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// pushRegion — push a rectangle of canvas pixels to the TFT.
// Clamps to canvas bounds automatically.
// ─────────────────────────────────────────────────────────────────────────────
void pushRegion(int x1, int y1, int x2, int y2) {
    x1 = constrain(x1, 0, CANVAS_W - 1);
    y1 = constrain(y1, 0, CANVAS_H - 1);
    x2 = constrain(x2, 0, CANVAS_W - 1);
    y2 = constrain(y2, 0, CANVAS_H - 1);
    if (x2 < x1 || y2 < y1) return;
    int pw = x2 - x1 + 1;
    tft.startWrite();
    for (int row = y1; row <= y2; row++) {
        tft.setAddrWindow(CANVAS_X + x1, CANVAS_Y + row, pw, 1);
        tft.pushPixels(&canvas[row * CANVAS_W + x1], pw);
    }
    tft.endWrite();
}

// ─────────────────────────────────────────────────────────────────────────────
// Shape drawing — all write into canvas[] only; caller must pushCanvas/pushRegion
// ─────────────────────────────────────────────────────────────────────────────

// Bresenham line into canvas (reuses the same algorithm as toolApplyLine but
// without brush stamping — just single pixels for clean shape outlines)
void canvasLine(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        if (x0 >= 0 && x0 < CANVAS_W && y0 >= 0 && y0 < CANVAS_H)
            canvasSet(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Line tool — draws with brush thickness (stamps circles along a Bresenham line)
void drawShapeLine(int x0, int y0, int x1, int y1, uint16_t color) {
    // For brush size 1, use pixel-perfect line; otherwise stamp circles
    if (brushSize <= 1) {
        canvasLine(x0, y0, x1, y1, color);
    } else {
        // Reuse toolApplyLine machinery: temporarily we just call it directly
        // since the active color and brush are already set
        toolApplyLine(x0, y0, x1, y1);
    }
}

// Rectangle outline — thickness = brushSize (draws inset filled bands)
void drawShapeRect(int x0, int y0, int x1, int y1, uint16_t color) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    int t = max(1, (int)brushSize - 1);  // border thickness in pixels
    for (int i = 0; i < t; i++) {
        int lx0 = x0 + i, ly0 = y0 + i, lx1 = x1 - i, ly1 = y1 - i;
        if (lx0 > lx1 || ly0 > ly1) break;
        canvasLine(lx0, ly0, lx1, ly0, color);  // top
        canvasLine(lx0, ly1, lx1, ly1, color);  // bottom
        canvasLine(lx0, ly0, lx0, ly1, color);  // left
        canvasLine(lx1, ly0, lx1, ly1, color);  // right
    }
}

// Midpoint ellipse algorithm — single outline ring, no lambdas
void drawEllipseRing(int cx, int cy, int rx, int ry, uint16_t color) {
    if (rx <= 0 || ry <= 0) {
        if (cx >= 0 && cx < CANVAS_W && cy >= 0 && cy < CANVAS_H)
            canvasSet(cx, cy, color);
        return;
    }
    long rx2 = (long)rx * rx, ry2 = (long)ry * ry;
    long twoRx2 = 2 * rx2, twoRy2 = 2 * ry2;
    long p, ex = 0, ey = ry;
    long ppx = 0, ppy = twoRx2 * ey;

    #define EP4(px2,py2) { \
        int _xs[4]={(int)(cx+(px2)),(int)(cx-(px2)),(int)(cx+(px2)),(int)(cx-(px2))}; \
        int _ys[4]={(int)(cy+(py2)),(int)(cy+(py2)),(int)(cy-(py2)),(int)(cy-(py2))}; \
        for(int _i=0;_i<4;_i++) \
            if(_xs[_i]>=0&&_xs[_i]<CANVAS_W&&_ys[_i]>=0&&_ys[_i]<CANVAS_H) \
                canvasSet(_xs[_i],_ys[_i],color); \
    }

    p = (long)ry2 - (long)rx2 * ry + (long)(rx2) / 4;
    while (ppx < ppy) {
        EP4(ex, ey);
        ex++;
        ppx += twoRy2;
        if (p < 0) {
            p += ry2 + ppx;
        } else {
            ey--;
            ppy -= twoRx2;
            p += ry2 + ppx - ppy;
        }
    }
    p = (long)ry2 * (ex + 1) * (ex + 1) + (long)rx2 * (ey - 1) * (ey - 1) - (long)rx2 * ry2;
    while (ey >= 0) {
        EP4(ex, ey);
        ey--;
        ppy -= twoRx2;
        if (p > 0) {
            p += rx2 - ppy;
        } else {
            ex++;
            ppx += twoRy2;
            p += rx2 - ppy + ppx;
        }
    }
    #undef EP4
}

// Ellipse outline — thickness = brushSize (draws concentric rings shrinking inward)
void drawShapeEllipse(int cx, int cy, int rx, int ry, uint16_t color) {
    int t = max(1, (int)brushSize - 1);
    for (int i = 0; i < t; i++) {
        int irx = rx - i, iry = ry - i;
        if (irx <= 0 || iry <= 0) {
            // Fill remaining centre pixel
            if (cx >= 0 && cx < CANVAS_W && cy >= 0 && cy < CANVAS_H)
                canvasSet(cx, cy, color);
            break;
        }
        drawEllipseRing(cx, cy, irx, iry, color);
    }
}
// Scanline flood fill — span queue algorithm.
// Processes whole horizontal spans instead of individual pixels, so the queue
// stays tiny (max ~800 spans for a 200x200 canvas) while covering all 40,000
// pixels reliably.  No heap allocation — uses a fixed static circular queue.
void floodFill(int x, int y, uint16_t newColor) {
    if (x < 0 || x >= CANVAS_W || y < 0 || y >= CANVAS_H) return;
    uint16_t oldColor = canvasGet(x, y);
    if (oldColor == newColor) return;

    // Each queue entry encodes one horizontal span to process:
    //   x1  — leftmost pixel of span
    //   x2  — rightmost pixel of span
    //   row — the row this span is on
    //   dy  — which neighbour rows to check (+1, -1, or both)
    struct Span { int16_t x1, x2, row, dy; };

    // 800 entries is more than enough for 200-wide canvas (max ~400 active spans)
    static Span q[800];
    int head = 0, tail = 0;
    #define Q_PUSH(a,b,r,d) { q[tail] = {(int16_t)(a),(int16_t)(b),(int16_t)(r),(int16_t)(d)}; tail = (tail+1)%800; }
    #define Q_POP(s)        { s = q[head]; head = (head+1)%800; }
    #define Q_EMPTY         (head == tail)

    // Scan right and left from seed to find the initial span
    int lx = x, rx = x;
    while (lx > 0            && canvasGet(lx-1, y) == oldColor) lx--;
    while (rx < CANVAS_W-1   && canvasGet(rx+1, y) == oldColor) rx++;
    // Paint it
    for (int i = lx; i <= rx; i++) canvasSet(i, y, newColor);
    // Seed both neighbour rows
    if (y > 0)            Q_PUSH(lx, rx, y-1, -1)
    if (y < CANVAS_H-1)   Q_PUSH(lx, rx, y+1, +1)

    while (!Q_EMPTY) {
        Span s;
        Q_POP(s)
        // Walk along this span's range looking for unpainted segments
        int i = s.x1;
        while (i <= s.x2) {
            // Skip already-painted or wrong-colour pixels
            if (canvasGet(i, s.row) != oldColor) { i++; continue; }
            // Found start of a new segment — extend it
            int segL = i;
            while (i <= s.x2 && canvasGet(i, s.row) == oldColor) {
                canvasSet(i, s.row, newColor);
                i++;
            }
            int segR = i - 1;
            // Extend segment beyond the parent span (handles concave shapes)
            int xl = segL, xr = segR;
            while (xl > 0          && canvasGet(xl-1, s.row) == oldColor) { xl--; canvasSet(xl, s.row, newColor); }
            while (xr < CANVAS_W-1 && canvasGet(xr+1, s.row) == oldColor) { xr++; canvasSet(xr, s.row, newColor); }
            // Push this segment's neighbour in the same direction
            int nr = s.row + s.dy;
            if (nr >= 0 && nr < CANVAS_H)
                Q_PUSH(xl, xr, nr, s.dy)
            // Also push the opposite-direction row if segment extended beyond parent
            int pr = s.row - s.dy;
            if (pr >= 0 && pr < CANVAS_H && (xl < s.x1 || xr > s.x2))
                Q_PUSH(xl, xr, pr, -s.dy)
        }
    }
    #undef Q_PUSH
    #undef Q_POP
    #undef Q_EMPTY
}

// ─────────────────────────────────────────────────────────────────────────────
// Main touch handler
// ─────────────────────────────────────────────────────────────────────────────
void handleMainTouch(int tx, int ty) {
    // Mirror-X toggle (left strip gap, y=200..219)
    if (rectHit(tx, ty, 0, 200, 20, 20)) {
        mirrorX = !mirrorX;
        drawLeftStrip();
        return;
    }

    // Mirror-Y toggle (right strip gap, y=200..219)
    if (rectHit(tx, ty, 220, 200, 20, 20)) {
        mirrorY = !mirrorY;
        drawRightStrip();
        return;
    }

    // Left strip (preset colors)
    if (rectHit(tx, ty, 0, 0, 20, CANVAS_H)) {
        int idx = ty / (CANVAS_H / 12);
        idx = constrain(idx, 0, 11);
        activeColor = PRESET_COLORS[idx];
        drawBottomBar();
        return;
    }

    // Right strip (custom slots)
    if (rectHit(tx, ty, 220, 0, 20, CANVAS_H)) {
        int idx = ty / (CANVAS_H / 12);
        idx = constrain(idx, 0, 11);
        // Short tap = select color, long tap handled via button
        activeColor = customColors[idx];
        drawBottomBar();
        return;
    }

    // Canvas area — only two-tap shape tools reach here via handleMainTouch.
    // Fill/Eyedropper are handled in loop() before debounce check.
    // Continuous tools (pen/eraser/spray) are also handled in loop().
    if (rectHit(tx, ty, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H)) {
        if (isTwoTapTool(activeTool)) {
            int cx = tx - CANVAS_X;
            int cy = ty - CANVAS_Y;

            if (!shapeHasOrigin) {
                shapeOriginX   = cx;
                shapeOriginY   = cy;
                shapeHasOrigin = true;
                int ox = CANVAS_X + cx, oy = CANVAS_Y + cy;
                tft.drawLine(ox - 5, oy, ox + 5, oy, C_ORANGE);
                tft.drawLine(ox, oy - 5, ox, oy + 5, C_ORANGE);
                drawBottomBar();
            } else {
                // Second tap: draw shape into canvas[], then push
                shapeHasOrigin = false;
                int ox = shapeOriginX, oy = shapeOriginY;
                int mx0 = CANVAS_W-1-ox, mx1 = CANVAS_W-1-cx;
                int my0 = CANVAS_H-1-oy, my1 = CANVAS_H-1-cy;

                auto drawShape = [&](int ax0, int ay0, int ax1, int ay1) {
                    if (activeTool == TOOL_LINE) {
                        drawShapeLine(ax0, ay0, ax1, ay1, activeColor);
                    } else if (activeTool == TOOL_RECT) {
                        drawShapeRect(ax0, ay0, ax1, ay1, activeColor);
                    } else if (activeTool == TOOL_ELLIPSE) {
                        int erx = abs(ax1 - ax0) / 2;
                        int ery = abs(ay1 - ay0) / 2;
                        int ecx = (ax0 + ax1) / 2;
                        int ecy = (ay0 + ay1) / 2;
                        drawShapeEllipse(ecx, ecy, erx, ery, activeColor);
                    }
                };

                drawShape(ox,  oy,  cx,  cy);
                if (mirrorX)           drawShape(mx0, oy,  mx1, cy);
                if (mirrorY)           drawShape(ox,  my0, cx,  my1);
                if (mirrorX && mirrorY) drawShape(mx0, my0, mx1, my1);

                pushCanvas();
                drawBottomBar();
            }
        }
        // (Fill, Eyedrop, Pen, Eraser, Spray all handled in loop() — nothing to do here)
        return;
    }

    // Bottom bar
    if (ty < BOTTOM_Y) return;
    int ry = ty - BOTTOM_Y;

    // Row 0: tool navigator  [<] [PEN] [>]  + name label + color swatch + preview
    if (ry < 30) {
        // [<] prev tool
        if (rectHit(tx, ry, 0, 0, 36, 30)) {
            activeTool = (Tool)((activeTool + TOOL_COUNT - 1) % TOOL_COUNT);
            shapeHasOrigin = false;
            drawBottomBar();
            return;
        }
        // [PEN] reset to pen
        if (rectHit(tx, ry, 36, 0, 36, 30)) {
            activeTool = TOOL_PEN;
            shapeHasOrigin = false;
            drawBottomBar();
            return;
        }
        // [>] next tool
        if (rectHit(tx, ry, 72, 0, 36, 30)) {
            activeTool = (Tool)((activeTool + 1) % TOOL_COUNT);
            shapeHasOrigin = false;
            drawBottomBar();
            return;
        }
        return;
    }

    // Row 1
    if (ry < 60) {
        int localY = ry - 30;
        // -Sz
        if (rectHit(tx, localY, 0, 0, 30, 30)) {
            if (brushSize > 1) brushSize--;
            drawBottomBar();
            return;
        }
        // +Sz
        if (rectHit(tx, localY, 30, 0, 30, 30)) {
            if (brushSize < 8) brushSize++;
            drawBottomBar();
            return;
        }
        // COLOUR PICK
        if (rectHit(tx, localY, 80, 0, 160, 30)) {
            cpSlot = 0;
            uint8_t r, g, b;
            rgb565to888(activeColor, r, g, b);
            cpR = r; cpG = g; cpB = b;
            drawColorPickScreen();
            return;
        }
        return;
    }

    // Row 2
    if (ry < 90) {
        int localY = ry - 60;
        if (rectHit(tx, localY, 0, 0, 60, 30)) {
            saveFile();
            drawBottomBar();
            return;
        }
        if (rectHit(tx, localY, 60, 0, 60, 30)) {
            scanFiles();
            drawGalleryScreen();
            return;
        }
        if (rectHit(tx, localY, 120, 0, 60, 30)) {
            confirmAction = CA_NEW;
            drawConfirmScreen("New canvas? Unsaved work lost.");
            return;
        }
        if (rectHit(tx, localY, 180, 0, 60, 30)) {
            confirmAction = CA_CLR;
            drawConfirmScreen("Clear to white?");
            return;
        }
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Colour Picker Screen
// ─────────────────────────────────────────────────────────────────────────────
void drawColorPickScreen() {
    currentScreen = SCR_COLOR_PICK;
    tft.fillScreen(0x1082);
    tft.setTextColor(C_WHITE, 0x1082);
    tft.setTextSize(1);
    tft.setCursor(4, 4);
    tft.print("COLOUR PICKER");

    // RGB sliders (horizontal bars)
    // R slider at y=30, G at y=80, B at y=130
    // Each 220px wide, mapped 0-255
    auto drawSlider = [&](int y, uint8_t val, uint16_t barColor, const char *lbl) {
        tft.drawString(lbl, 2, y);
        tft.fillRect(20, y, 200, 20, 0x2104);
        int filled = map(val, 0, 255, 0, 200);
        tft.fillRect(20, y, filled, 20, barColor);
        tft.drawRect(20, y, 200, 20, C_WHITE);
        tft.setTextColor(C_WHITE, 0x2104);
        char buf[5]; sprintf(buf, "%3d", val);
        tft.drawString(buf, 222, y + 6);
    };

    drawSlider(30,  cpR, C_RED,   "R");
    drawSlider(80,  cpG, C_GREEN, "G");
    drawSlider(130, cpB, C_BLUE,  "B");

    // Preview
    uint16_t preview = rgb888to565(cpR, cpG, cpB);
    tft.fillRect(20, 160, 80, 50, preview);
    tft.drawRect(20, 160, 80, 50, C_WHITE);
    tft.setTextColor(C_WHITE, 0x1082);
    tft.drawString("Preview", 22, 215);

    // Slot selector (which custom slot to assign to)
    tft.drawString("Slot:", 110, 165);
    char slotBuf[4]; sprintf(slotBuf, "%2d", cpSlot + 1);
    tft.setTextSize(2);
    tft.setTextColor(C_YELLOW, 0x1082);
    tft.drawString(slotBuf, 145, 160);
    tft.setTextSize(1);
    tft.setTextColor(C_WHITE, 0x1082);
    drawButtonRect(110, 178, 30, 20, 0x4208, C_WHITE, "<");
    drawButtonRect(145, 178, 30, 20, 0x4208, C_WHITE, ">");

    // Buttons
    drawButtonRect(10,  270, 100, 36, C_GREEN,  C_BLACK, "OK");
    drawButtonRect(130, 270, 100, 36, 0x4208,   C_WHITE, "CANCEL");

    // Use as active
    drawButtonRect(10, 230, 220, 36, 0x4208, C_YELLOW, "USE AS ACTIVE COLOR");
}

void handleColorPickTouch(int tx, int ty) {
    // R slider
    if (rectHit(tx, ty, 20, 30, 200, 20)) {
        cpR = map(tx - 20, 0, 200, 0, 255);
        drawColorPickScreen(); return;
    }
    // G slider
    if (rectHit(tx, ty, 20, 80, 200, 20)) {
        cpG = map(tx - 20, 0, 200, 0, 255);
        drawColorPickScreen(); return;
    }
    // B slider
    if (rectHit(tx, ty, 20, 130, 200, 20)) {
        cpB = map(tx - 20, 0, 200, 0, 255);
        drawColorPickScreen(); return;
    }
    // Slot < >
    if (rectHit(tx, ty, 110, 178, 30, 20)) {
        cpSlot = (cpSlot + 11) % 12;
        drawColorPickScreen(); return;
    }
    if (rectHit(tx, ty, 145, 178, 30, 20)) {
        cpSlot = (cpSlot + 1) % 12;
        drawColorPickScreen(); return;
    }
    // Use as active
    if (rectHit(tx, ty, 10, 230, 220, 36)) {
        activeColor = rgb888to565(cpR, cpG, cpB);
        drawMainScreen(); return;
    }
    // OK — save to slot and set active
    if (rectHit(tx, ty, 10, 270, 100, 36)) {
        customColors[cpSlot] = rgb888to565(cpR, cpG, cpB);
        activeColor = customColors[cpSlot];
        drawMainScreen(); return;
    }
    // Cancel
    if (rectHit(tx, ty, 130, 270, 100, 36)) {
        drawMainScreen(); return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Gallery Screen
// ─────────────────────────────────────────────────────────────────────────────
void scanFiles() {
    galleryCount = 0;
    if (!sdOK) return;
    File root = SD.open("/");
    while (galleryCount < 32) {
        File f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            const char *nm = f.name();
            int len = strlen(nm);
            if (len > 4 && strcasecmp(nm + len - 4, ".bmp") == 0) {
                strncpy(galleryFiles[galleryCount], nm, 12);
                galleryFiles[galleryCount][12] = 0;
                galleryCount++;
            }
        }
        f.close();
    }
    root.close();
}

void countFiles() {
    fileCount = 0;
    if (!sdOK) return;
    File root = SD.open("/");
    while (true) {
        File f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            const char *nm = f.name();
            int len = strlen(nm);
            if (len > 4 && strcasecmp(nm + len - 4, ".bmp") == 0) fileCount++;
        }
        f.close();
    }
    root.close();
}

void drawGalleryScreen() {
    currentScreen = SCR_GALLERY;
    tft.fillScreen(0x1082);
    tft.setTextColor(C_WHITE, 0x1082);
    tft.setTextSize(1);
    tft.setCursor(4, 4);
    tft.printf("GALLERY  Page %d/%d", galleryPage + 1, max(1, (galleryCount + 3) / 4));

    // 2x2 thumbnails, each ~100x110
    int startIdx = galleryPage * 4;
    for (int i = 0; i < 4 && startIdx + i < galleryCount; i++) {
        int col = i % 2, row = i / 2;
        int tx = col * 120, ty = 18 + row * 140;
        tft.drawRect(tx, ty, 110, 110, C_WHITE);
        tft.setTextColor(C_GRAY, 0x1082);
        tft.setCursor(tx + 2, ty + 112);
        tft.print(galleryFiles[startIdx + i]);

        // Draw mini load/del buttons
        drawButtonRect(tx,      ty + 124, 55, 20, 0x4208, C_GREEN, "LOAD");
        drawButtonRect(tx + 55, ty + 124, 55, 20, 0x4208, C_RED,   "DEL");
    }

    // Nav buttons
    drawButtonRect(0,   300, 60, 20, 0x4208, C_WHITE, "<PREV");
    drawButtonRect(90,  300, 60, 20, 0x4208, C_WHITE, "NEXT>");
    drawButtonRect(180, 300, 60, 20, 0x4208, C_ORANGE, "BACK");
}

void handleGalleryTouch(int tx, int ty) {
    // Back
    if (rectHit(tx, ty, 180, 300, 60, 20)) {
        drawMainScreen(); return;
    }
    // Prev page
    if (rectHit(tx, ty, 0, 300, 60, 20)) {
        if (galleryPage > 0) { galleryPage--; drawGalleryScreen(); }
        return;
    }
    // Next page
    if (rectHit(tx, ty, 90, 300, 60, 20)) {
        if ((galleryPage + 1) * 4 < galleryCount) { galleryPage++; drawGalleryScreen(); }
        return;
    }

    // Thumbnails (LOAD / DEL)
    int startIdx = galleryPage * 4;
    for (int i = 0; i < 4 && startIdx + i < galleryCount; i++) {
        int col = i % 2, row = i / 2;
        int bx = col * 120, by = 18 + row * 140;
        if (rectHit(tx, ty, bx, by + 124, 55, 20)) {
            // Load
            loadFile(startIdx + i);
            drawMainScreen();
            return;
        }
        if (rectHit(tx, ty, bx + 55, by + 124, 55, 20)) {
            // Confirm delete
            confirmAction = CA_DEL_FILE;
            confirmFileIdx = startIdx + i;
            char msg[40];
            snprintf(msg, 40, "Delete %s?", galleryFiles[startIdx + i]);
            drawConfirmScreen(msg);
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Confirm Screen
// ─────────────────────────────────────────────────────────────────────────────
void drawConfirmScreen(const char *msg) {
    currentScreen = SCR_CONFIRM;
    tft.fillScreen(0x1082);
    tft.setTextColor(C_WHITE, 0x1082);
    tft.setTextSize(1);
    tft.setCursor(10, 100);
    tft.println(msg);
    drawButtonRect(20,  200, 90, 40, C_GREEN, C_BLACK, "YES");
    drawButtonRect(130, 200, 90, 40, C_RED,   C_WHITE, "NO");
}

void handleConfirmTouch(int tx, int ty) {
    bool yes = rectHit(tx, ty, 20, 200, 90, 40);
    bool no  = rectHit(tx, ty, 130, 200, 90, 40);
    if (!yes && !no) return;

    if (yes) {
        switch (confirmAction) {
            case CA_NEW:
                clearCanvas(C_WHITE);
                break;
            case CA_CLR:
                clearCanvas(C_WHITE);
                break;
            case CA_DEL_FILE:
                if (sdOK && confirmFileIdx >= 0 && confirmFileIdx < galleryCount) {
                    char path[16];
                    snprintf(path, 16, "/%s", galleryFiles[confirmFileIdx]);
                    SD.remove(path);
                    countFiles();
                }
                break;
        }
    }

    if (confirmAction == CA_DEL_FILE && yes) {
        scanFiles();
        drawGalleryScreen();
    } else {
        drawMainScreen();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BMP Save / Load
// ─────────────────────────────────────────────────────────────────────────────
// BMP is stored bottom-up; rows padded to 4-byte boundary.
// 200*3 = 600 bytes per row — already a multiple of 4, so no padding.

void saveFile() {
    if (!sdOK) return;
    // Find next free filename PAINT_NNN.bmp
    char path[20];
    for (int n = 0; n < 1000; n++) {
        snprintf(path, 20, "/PAINT%03d.BMP", n);
        if (!SD.exists(path)) break;
    }

    File f = SD.open(path, FILE_WRITE);
    if (!f) return;

    uint32_t rowStride = CANVAS_W * 3;  // 600, multiple of 4
    uint32_t dataSize  = rowStride * CANVAS_H;
    uint32_t fileSize  = 54 + dataSize;

    // BMP file header (14 bytes)
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = fileSize & 0xFF; hdr[3] = (fileSize >> 8) & 0xFF;
    hdr[4] = (fileSize >> 16) & 0xFF; hdr[5] = (fileSize >> 24) & 0xFF;
    hdr[10] = 54; // pixel data offset
    // DIB header (40 bytes)
    hdr[14] = 40; // header size
    hdr[18] = CANVAS_W & 0xFF; hdr[19] = (CANVAS_W >> 8) & 0xFF;
    hdr[22] = CANVAS_H & 0xFF; hdr[23] = (CANVAS_H >> 8) & 0xFF;
    hdr[26] = 1;   // color planes
    hdr[28] = 24;  // bits per pixel
    hdr[34] = dataSize & 0xFF; hdr[35] = (dataSize >> 8) & 0xFF;
    hdr[36] = (dataSize >> 16) & 0xFF; hdr[37] = (dataSize >> 24) & 0xFF;
    f.write(hdr, 54);

    // Write rows bottom-up
    for (int row = CANVAS_H - 1; row >= 0; row--) {
        for (int col = 0; col < CANVAS_W; col++) {
            uint16_t c = canvasGet(col, row);
            uint8_t r, g, b;
            rgb565to888(c, r, g, b);
            rowBuf[col * 3 + 0] = b;
            rowBuf[col * 3 + 1] = g;
            rowBuf[col * 3 + 2] = r;
        }
        f.write(rowBuf, rowStride);
    }
    f.close();
    countFiles();
}

void loadFile(int idx) {
    if (!sdOK || idx < 0 || idx >= galleryCount) return;
    char path[16];
    snprintf(path, 16, "/%s", galleryFiles[idx]);
    File f = SD.open(path);
    if (!f) return;

    // Skip 54-byte BMP header
    f.seek(54);

    // Rows stored bottom-up in BMP
    uint32_t rowStride = CANVAS_W * 3;
    for (int row = CANVAS_H - 1; row >= 0; row--) {
        if (f.read(rowBuf, rowStride) != (int)rowStride) break;
        for (int col = 0; col < CANVAS_W; col++) {
            uint8_t b = rowBuf[col * 3 + 0];
            uint8_t g = rowBuf[col * 3 + 1];
            uint8_t r = rowBuf[col * 3 + 2];
            canvasSet(col, row, rgb888to565(r, g, b));
        }
    }
    f.close();
}
