// CYD Paint v24 — ESP32-2432S028 (ILI9341 240x320, XPT2046, SD)
// v24: Copy/cut select, dup frame, recent colors, color replace, flip sel H/V,
//      ping-pong playback, per-frame delay, line stabilizer, grid overlay,
//      cheap onion skin (row-by-row), native .CYD project format, pattern brushes,
//      magic wand selection, fill tolerance, text tool, optimizations
// TFT via TFT_eSPI (configure User_Setup.h externally)
// Touch on HSPI (CLK=25, MISO=39, MOSI=32, CS=33, IRQ=36)
// SD on VSPI (CLK=18, MISO=19, MOSI=23, CS=5) — default VSPI pins
// Canvas 200x200 heap-allocated, placed at x=20, y=20; status bar in top gap x=20..219 y=0..19

struct UndoSlot;
static void undoFreeSlot(UndoSlot *s);

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
#define STATUS_Y     0   // status bar in the top gap (y=0..19), between swatch strips
#define STATUS_H    20
#define BOTTOM_Y   230   // bottom bar starts here (100 px tall)
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

// UI chrome colours
#define C_BTN_BG    0x4208   // standard button background
#define C_BTN_BG_DK 0x2104   // dimmed / disabled button background
#define C_BAR_BG    0x2104   // bottom bar background
#define C_CUT_BG    0x8000   // cut-mode indicator (dark red)
#define C_COPY_BG   0x0400   // copy-mode indicator (dark green)

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
    TOOL_SEL_RECT,
    TOOL_LASSO,
    TOOL_WAND,         // magic wand flood-select
    TOOL_COL_REPLACE,  // replace all matching color
    TOOL_GRADIENT,     // gradient fill (2-tap: direction)
    TOOL_POLYGON,      // polygon (multi-tap, double-tap closes)
    TOOL_CURVE,        // bezier curve (3-tap: start, end, control)
    TOOL_TEXT,         // text stamp (tap to place, on-screen keyboard)
    TOOL_COUNT   // sentinel — keep last
};

// Continuous-draw tools: bypass debounce, interpolate strokes
inline bool isContinuousTool(Tool t) {
    return t == TOOL_PEN || t == TOOL_ERASER || t == TOOL_SPRAY;
}
// Two-tap tools: first tap = origin, second tap = commit
inline bool isTwoTapTool(Tool t) {
    return t == TOOL_LINE || t == TOOL_RECT || t == TOOL_ELLIPSE || t == TOOL_GRADIENT;
}
// Single-tap canvas tools with their own debounce (not UI debounce)
inline bool isSingleTapTool(Tool t) {
    return t == TOOL_FILL || t == TOOL_EYEDROP || t == TOOL_COL_REPLACE || t == TOOL_WAND || t == TOOL_TEXT;
}
// Multi-phase tools (polygon, curve)
inline bool isMultiTapTool(Tool t) {
    return t == TOOL_POLYGON || t == TOOL_CURVE;
}
// Selection tools — have their own multi-phase handling
inline bool isSelectionTool(Tool t) {
    return t == TOOL_SEL_RECT || t == TOOL_LASSO;
}

// ── Animation Constants ───────────────────────────────────────────────────────
#define MAX_FRAMES         8      // up to 8 frames — RLE compressed, not full buffers
#define ANIM_MAX_RLE_BYTES 24000  // cap per compressed frame (~24KB); refuse +FRM if exceeded
#define ANIM_THUMB_W      22
#define ANIM_THUMB_H      18
#define ANIM_STRIP_Y     222
#define ANIM_FPS_MIN       1
#define ANIM_FPS_MAX      12

// Compressed frame storage: RLE-encoded RGB565.
// Format: [count:u8][pixLo:u8][pixHi:u8], count=1..255.
struct RleFrame { uint8_t *data; uint32_t size; };

// ── Screens ──────────────────────────────────────────────────────────────────
enum Screen { SCR_MAIN = 0, SCR_COLOR_PICK, SCR_GALLERY, SCR_CONFIRM, SCR_ANIM, SCR_RENAME, SCR_TOOL_SEL, SCR_BRUSH_SEL, SCR_TEXT_INPUT };

// ── Globals ──────────────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

uint16_t *canvas = nullptr;          // 200*200 * 2 bytes
uint16_t customColors[12];           // right strip custom slots
uint16_t activeColor  = C_BLACK;
uint16_t secondColor  = C_WHITE;  // v24: secondary color for gradients & shape fill
bool     constrainMode = false;   // v24: constrain shapes
bool     gradientRadial = false;  // v24: false=linear, true=radial gradient (rect→square, ellipse→circle, line→45°)
uint8_t  brushSize    = 3;
Tool     activeTool   = TOOL_PEN;

// Display order for tool select screen (maps grid position → enum value)
const Tool toolDisplayOrder[] = {
    TOOL_PEN, TOOL_ERASER, TOOL_SPRAY, TOOL_FILL, TOOL_EYEDROP, TOOL_COL_REPLACE,
    TOOL_GRADIENT, TOOL_LINE, TOOL_RECT, TOOL_ELLIPSE, TOOL_POLYGON, TOOL_CURVE,
    TOOL_SEL_RECT, TOOL_LASSO, TOOL_WAND, TOOL_TEXT
};
// Display names (matching toolDisplayOrder)
const char *toolDisplayNames[] = {
    "Pen", "Eraser", "Spray", "Fill", "EyeDrop", "ColRepl",
    "Gradnt", "Line", "Rect", "Ellipse", "Polygon", "Curve",
    "SelRect", "Lasso", "Wand", "Text"
};
const uint16_t toolDisplayColors[] = {
    C_WHITE, C_GRAY, C_ORANGE, C_GREEN, C_YELLOW, C_RED,
    C_LIME, C_CYAN, C_CYAN, C_CYAN, C_CYAN, C_CYAN,
    C_MAGENTA, C_MAGENTA, C_MAGENTA, C_YELLOW
};
Screen   currentScreen = SCR_MAIN;
bool     sdOK         = false;
int      fileCount    = 0;

// Pattern brush mode (0=solid, 1=checker, 2=dots, 3=hlines, 4=vlines, 5=diag)
uint8_t  patternMode = 0;
#define PATTERN_COUNT 6

// Fill tolerance (0=exact, higher=fuzzier)
uint8_t  fillTolerance = 0;

// Line stabilizer (0=off, 1=light, 2=heavy)
uint8_t  stabilizerLevel = 0;

// Grid overlay (visible at zoom)
bool     gridOverlay = false;

// Copy vs Cut selection mode
bool     selCutMode = true;

// ── Animation state ───────────────────────────────────────────────────────────
// Only canvas[] (one full buffer) exists in RAM. All other frames are RLE-compressed.
// Switching frames: compress current → animStore; decompress target → canvas.
RleFrame  animStore[MAX_FRAMES];  // compressed storage; current frame slot is always empty
int       animFrameCount = 1;
int       animCurFrame   = 0;
int       animFPS        = 6;
bool      animPlaying    = false;
unsigned long animLastFrameTime = 0;
int       animStripScroll = 0;

// Per-frame delay in ms (0 = use global FPS)
uint16_t  animFrameDelay[MAX_FRAMES];

// Ping-pong playback
bool      animPingPong = false;
int       animPlayDir  = 1;

// Onion skin
bool      onionSkin = false;

// Line stabilizer accumulator
float     stabX = 0, stabY = 0;

// Selection tool state
enum SelPhase { SEL_NONE = 0, SEL_CORNER1, SEL_CORNER2, SEL_PLACE, SEL_LASSO_DRAW, SEL_LASSO_PLACE };
SelPhase  selPhase = SEL_NONE;
int       selX1 = 0, selY1 = 0, selX2 = 0, selY2 = 0;   // selected rect bounds (canvas coords)
uint16_t *selBuf   = nullptr;     // pixel buffer for selected region
uint8_t  *selMask  = nullptr;     // 1-bit mask for lasso (packed bits)
int       selW = 0, selH = 0;     // dimensions of selection
// Lasso outline points (canvas coords) — stored as pairs
#define LASSO_MAX_PTS 600
int16_t   lassoX[LASSO_MAX_PTS], lassoY[LASSO_MAX_PTS];
int       lassoPtCount = 0;

// Frame reorder state
enum AnimOp { AOP_NONE = 0, AOP_MOVE, AOP_SWAP };
AnimOp animOp = AOP_NONE;        // active reorder operation
int    animOpSrcFrame = -1;       // source frame for move/swap

// Confirm screen state
enum ConfirmAction { CA_NEW, CA_CLR, CA_DEL_FILE, CA_DEL_ANIM, CA_BATCH_DEL };
ConfirmAction confirmAction;
int    confirmFileIdx = -1;     // for delete

// Status bar message (shown briefly above canvas)
char         statusMsg[32] = "";
unsigned long statusExpiry = 0;   // millis() when message should vanish
int          imgCount  = 0;       // standalone PAINT images

int          cydCount  = 0;       // v24: native .CYD project files

// Gallery state
int  galleryPage = 0;
char galleryFiles[128][16];     // image mode: filename e.g. "PAINT000.BMP"
                                // anim  mode: sequence prefix e.g. "ANM000"
int  galleryCount = 0;
int  galleryMode = 0;  // 0=images, 1=CYD projects
bool gallerySelMode   = false;  // batch-select mode active?
bool gallerySel[128];           // which items are selected for batch ops

// Rename screen state
enum Screen2 { SCR2_NONE = 0, SCR2_RENAME };
Screen2 screen2 = SCR2_NONE;
int     renameIdx = -1;
char    renameBuf[16] = "";
int     renameCursor = 0;

// Color picker state
int cpSlot = -1;                // which custom slot we're editing
uint8_t cpR = 255, cpG = 0, cpB = 0;

// Touch debounce (UI elements only — drawing bypasses this)
unsigned long lastTouch = 0;
unsigned long lastCanvasTap = 0;   // separate debounce for Fill/Eyedropper/Shape taps
#define TOUCH_DEBOUNCE_MS    150
#define CANVAS_DEBOUNCE_MS   150   // slightly longer — prevents double-fill on tap

// Smooth drawing state
int  lastDrawX = -1;   // previous canvas-space X (-1 = no previous point)
int  lastDrawY = -1;
bool wasDrawing = false;  // was the stylus on the canvas last frame?

// Mirror mode state
bool mirrorX = false;   // mirror horizontally (flip across vertical centre line)
bool mirrorY = false;   // mirror vertically   (flip across horizontal centre line)

// Zoom state
int    zoomLevel    = 0;       // 0=1×, 1=2×, 2=4×, 3=8×, 4=16×
#define ZOOM_MAX_LEVEL 4
bool   zoomPending  = false;   // first tap done, waiting for second?
// Derived viewport (canvas coords visible on screen) — recomputed on zoom in/out
int    zoomVX = 0, zoomVY = 0; // top-left of visible region in canvas space
int    zoomVW = CANVAS_W;      // width  of visible region (CANVAS_W when not zoomed)
int    zoomVH = CANVAS_H;      // height of visible region

// Two-tap shape state
bool shapeHasOrigin = false;  // waiting for second tap?
int  shapeOriginX   = 0;
int  shapeOriginY   = 0;

// Polygon/curve tool state
#define POLY_MAX_PTS 32
int      polyPtCount = 0;
int16_t  polyX[POLY_MAX_PTS];
int16_t  polyY[POLY_MAX_PTS];
unsigned long polyLastTap = 0;
uint8_t  curvePhase = 0;  // TOOL_CURVE tap phase (0..2)
int      curveX0 = 0, curveY0 = 0, curveX1 = 0, curveY1 = 0;

// Text tool state
#define TEXT_MAX_LEN 128
char     textBuf[TEXT_MAX_LEN + 1] = "Hello";
int      textLen = 5;
int      textCursor = 5;
uint8_t  textFont = 0;       // 0=normal, 1=bold, 2=wide
#define TEXT_FONT_COUNT 3
bool     textLowerCase = false;


// Shared static row buffer for BMP I/O (max 200 pixels × 3 bytes = 600)
static uint8_t rowBuf[CANVAS_W * 3];

// Undo/Redo — RLE-compressed canvas snapshots
#define UNDO_MAX_SLOTS   20
#define UNDO_HEAP_RESERVE 20000  // keep at least 20KB free
struct UndoSlot { uint8_t *data; uint32_t size; };
UndoSlot undoStack[UNDO_MAX_SLOTS];
int      undoCount = 0;   // number of valid undo states
int      undoTop   = 0;   // write position (circular)
UndoSlot redoStack[UNDO_MAX_SLOTS];
int      redoCount = 0;
int      redoTop   = 0;

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
void drawToolSelectScreen();
void handleToolSelectTouch(int tx, int ty);
void drawBrushSelectScreen();
void drawTextInputScreen();
void handleTextInputTouch(int tx, int ty);
void stampText(int x, int y);
void handleBrushSelectTouch(int tx, int ty);
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
void drawToggleBtn(int x, int y, int w, int h, bool active, uint16_t accentBg, const char *label);
void drawEnableBtn(int x, int y, int w, int h, bool enabled, const char *label);
bool rectHit(int tx, int ty, int x, int y, int w, int h);
uint16_t canvasGet(int x, int y);
void canvasSet(int x, int y, uint16_t c);
void pushCanvas();
void clearCanvas(uint16_t color);
// Animation
uint32_t rleCompress(const uint16_t *src, uint8_t *dst, uint32_t dstCap);
void rleDecompress(const uint8_t *src, uint32_t srcLen, uint16_t *dst);
bool animCompressAndStore(int slot);
void animLoadFromStore(int slot);
void animCommitFrame();
void animSyncToFrame(int frameIdx);
void animAddFrame();
void animDeleteFrame();
void animDrawThumb(int fi, int sx, int sy, int tw, int th, bool hi);
void animDrawFilmstrip();
void drawAnimScreen();
void handleAnimTouch(int tx, int ty);
void animPlay();
void animStop();
void animMoveFrame(int from, int to);
void animSwapFrames(int a, int b);
// Zoom
void zoomIn(int cx, int cy);
void zoomOut();
void pushCanvasZoomed();
int  screenToCanvasX(int sx);
int  screenToCanvasY(int sy);
// Status bar
void drawStatusBar();
void setStatus(const char *msg);
void updateStatusCounts();
// Gallery extras
void drawRenameScreen();
void handleRenameTouch(int tx, int ty);
void galleryClearSel();
// Undo/Redo
void undoPush();
void undoPerform();
void redoPerform();
void undoClearAll();
void redoClearAll();
// Selection
void selCancel();
void selFlipH();
void selFlipV();
void selRotate90();
// v24 helpers
int  colorDist565(uint16_t a, uint16_t b);
bool colorMatch(uint16_t a, uint16_t b);
bool colorMatch565(uint16_t a, uint16_t b);
bool patternCheck(int x, int y);
void wandSelect(int x, int y);
void drawGradientLine(int x0, int y0, int x1, int y1);
void gradientFillRegion(int seedX, int seedY, int x0, int y0, int x1, int y1);
void drawPolygon();
void drawBezierCurve(int x0, int y0, int x1, int y1, int cx, int cy);
void fillShapeRect(int x0, int y0, int x1, int y1);
void fillShapeEllipse(int cx, int cy, int rx, int ry);
void saveCYD();
void loadCYD(int idx);
void scanCYDs();
void animDuplicateFrame();
void selCaptureRect();
void selCaptureLasso();
void selPlace(int destX, int destY);
void selDrawOutline();
void selBuildLassoMask();
bool selMaskGet(int x, int y);

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

    // Allocate canvas (single working buffer — 80KB)
    size_t canvasSz = CANVAS_W * CANVAS_H * sizeof(uint16_t);
    canvas = (uint16_t *)heap_caps_malloc(canvasSz, MALLOC_CAP_8BIT);
    if (!canvas) canvas = (uint16_t *)malloc(canvasSz);
    if (!canvas) { tft.drawString("OUT OF MEMORY", 10, 150, 2); while (1) delay(1000); }
    Serial.printf("setup: canvas OK, heap free=%d\n", (int)ESP.getFreeHeap());

    for (size_t p = 0; p < CANVAS_W * CANVAS_H; p++) canvas[p] = C_WHITE;
    for (int i = 0; i < MAX_FRAMES; i++) { animStore[i].data = nullptr; animStore[i].size = 0; animFrameDelay[i] = 0; }
    for (int i = 0; i < UNDO_MAX_SLOTS; i++) { undoStack[i].data = nullptr; undoStack[i].size = 0; redoStack[i].data = nullptr; redoStack[i].size = 0; }
    animFrameCount = 1;
    animCurFrame   = 0;

    // Init custom colors and recent colors
    for (int i = 0; i < 12; i++) customColors[i] = C_WHITE;

    countFiles();
    updateStatusCounts();
    drawMainScreen();
}

void loop() {
    // ── Status bar message expiry ────────────────────────────────────────────
    if (statusMsg[0] && millis() > statusExpiry && currentScreen == SCR_MAIN) {
        statusMsg[0] = '\0';
        drawStatusBar();
    }

    // ── Animation playback tick ───────────────────────────────────────────────
    if (animPlaying && currentScreen == SCR_ANIM) {
        unsigned long now = millis();
        unsigned long frameInterval = (animFrameDelay[animCurFrame] > 0) ? animFrameDelay[animCurFrame] : (1000UL / animFPS);
        if (now - animLastFrameTime >= frameInterval) {
            animLastFrameTime = now;
            int next;
            if (animPingPong) {
                next = animCurFrame + animPlayDir;
                if (next >= animFrameCount) { animPlayDir = -1; next = animCurFrame - 1; }
                if (next < 0)               { animPlayDir = 1;  next = animCurFrame + 1; }
                if (next < 0 || next >= animFrameCount) next = 0;
            } else {
                next = (animCurFrame + 1) % animFrameCount;
            }
            animLoadFromStore(next);   // decompress next frame into canvas
            animCurFrame = next;
            tft.startWrite();
            tft.setAddrWindow(CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H);
            tft.pushPixels(canvas, CANVAS_W * CANVAS_H);
            tft.endWrite();
            tft.fillRect(0, 0, 110, 18, 0x1082);
            tft.setTextColor(C_YELLOW, 0x1082);
            tft.setTextSize(1);
            tft.setCursor(2, 4);
            tft.printf("ANIM F:%d/%d %dfps%s", animCurFrame + 1, animFrameCount, animFPS, animPingPong ? " PP" : "");
        }
    }
    bool touching = touch.tirqTouched() && touch.touched();

    if (!touching) {
        if (wasDrawing) {
            // If lasso was being drawn, complete it
            if (activeTool == TOOL_LASSO && selPhase == SEL_LASSO_DRAW && lassoPtCount >= 3) {
                undoPush();  // snapshot before lasso cut
                selCaptureLasso();
                selPhase = SEL_LASSO_PLACE;
                selDrawOutline();
                setStatus("Tap dest (inside=keep)");
            } else if (activeTool == TOOL_LASSO && selPhase == SEL_LASSO_DRAW) {
                // Too few points — cancel
                selPhase = SEL_NONE;
                lassoPtCount = 0;
                pushCanvas();
                setStatus("Lasso cancelled");
            }
            lastDrawX  = -1;
            lastDrawY  = -1;
            wasDrawing = false;
            if (currentScreen == SCR_MAIN) drawStatusBar();  // update undo count
        }
        return;
    }

    TS_Point p = touch.getPoint();
    int tx = map(p.x, 200, 3800, 0, 240);
    int ty = map(p.y, 200, 3800, 0, 320);
    tx = constrain(tx, 0, 239);
    ty = constrain(ty, 0, 319);

    // ── Touch sample averaging — reduces XPT2046 noise ───────────────────────
    // Take 3 rapid samples and average them. Discard if they spread too far
    // (usually means the stylus just lifted or landed — a transient spike).
    {
        TS_Point p2 = touch.getPoint();
        TS_Point p3 = touch.getPoint();
        // Raw average
        int rx = ((int)p.x + p2.x + p3.x) / 3;
        int ry = ((int)p.y + p2.y + p3.y) / 3;
        // Reject if any sample deviates more than 200 raw units (spike filter)
        int sx = max(abs((int)p.x - rx), max(abs((int)p2.x - rx), abs((int)p3.x - rx)));
        int sy = max(abs((int)p.y - ry), max(abs((int)p2.y - ry), abs((int)p3.y - ry)));
        if (sx > 200 || sy > 200) return;  // transient — skip this poll cycle
        tx = map(rx, 200, 3800, 0, 240);
        ty = map(ry, 200, 3800, 0, 320);
        tx = constrain(tx, 0, 239);
        ty = constrain(ty, 0, 319);
    }

    // ── Smooth drawing path — only on SCR_MAIN ────────────────────────────────
    if (currentScreen == SCR_MAIN && !zoomPending &&
        rectHit(tx, ty, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H) &&
        isContinuousTool(activeTool))
    {
        int cx, cy;
        if ((zoomLevel > 0)) {
            cx = screenToCanvasX(tx);
            cy = screenToCanvasY(ty);
        } else {
            cx = tx - CANVAS_X;
            cy = ty - CANVAS_Y;
        }
        cx = constrain(cx, 0, CANVAS_W - 1);
        cy = constrain(cy, 0, CANVAS_H - 1);

        // Line stabilizer — smooths resistive touch jitter
        if (stabilizerLevel > 0 && lastDrawX >= 0) {
            // Lower factor = heavier smoothing (more lag, smoother line)
            float factor = (stabilizerLevel == 1) ? 0.3f : 0.12f;
            stabX = stabX + ((float)cx - stabX) * factor;
            stabY = stabY + ((float)cy - stabY) * factor;
            cx = (int)(stabX + 0.5f);
            cy = (int)(stabY + 0.5f);
        } else {
            stabX = cx;
            stabY = cy;
        }

        if (lastDrawX < 0) {
            stabX = cx; stabY = cy;  // reset accumulator on new stroke
            undoPush();
            toolApply(cx, cy);
        } else {
            int dx = cx - lastDrawX, dy = cy - lastDrawY;
            int minDist = (stabilizerLevel > 0) ? 1 : ((zoomLevel > 0) ? 2 : 8);
            if (dx*dx + dy*dy < minDist) return;
            toolApplyLine(lastDrawX, lastDrawY, cx, cy);
        }
        lastDrawX  = cx;
        lastDrawY  = cy;
        wasDrawing = true;
        return;
    }

    // ── Lasso drawing — continuous point collection ─────────────────────────
    if (currentScreen == SCR_MAIN && !zoomPending &&
        rectHit(tx, ty, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H) &&
        activeTool == TOOL_LASSO && selPhase == SEL_LASSO_DRAW)
    {
        int cx, cy;
        if ((zoomLevel > 0)) {
            cx = screenToCanvasX(tx);
            cy = screenToCanvasY(ty);
        } else {
            cx = tx - CANVAS_X;
            cy = ty - CANVAS_Y;
        }
        cx = constrain(cx, 0, CANVAS_W - 1);
        cy = constrain(cy, 0, CANVAS_H - 1);

        // Add point if moved enough and have room
        if (lassoPtCount < LASSO_MAX_PTS) {
            if (lassoPtCount == 0 || abs(cx - lassoX[lassoPtCount-1]) > 1 || abs(cy - lassoY[lassoPtCount-1]) > 1) {
                lassoX[lassoPtCount] = cx;
                lassoY[lassoPtCount] = cy;
                lassoPtCount++;
                // Draw point on screen as visual feedback
                int sx2, sy2;
                if ((zoomLevel > 0)) {
                    sx2 = CANVAS_X + (cx - zoomVX) * CANVAS_W / zoomVW;
                    sy2 = CANVAS_Y + (cy - zoomVY) * CANVAS_H / zoomVH;
                } else {
                    sx2 = CANVAS_X + cx;
                    sy2 = CANVAS_Y + cy;
                }
                tft.drawPixel(sx2, sy2, C_YELLOW);
            }
        }
        wasDrawing = true;
        return;
    }

    // ── Selection tap handling (rect corner1/corner2/place, lasso place) ────
    if (currentScreen == SCR_MAIN && !zoomPending &&
        rectHit(tx, ty, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H) &&
        isSelectionTool(activeTool) && selPhase != SEL_LASSO_DRAW)
    {
        unsigned long now = millis();
        if (now - lastCanvasTap < CANVAS_DEBOUNCE_MS) return;
        lastCanvasTap = now;

        int cx, cy;
        if ((zoomLevel > 0)) {
            cx = screenToCanvasX(tx);
            cy = screenToCanvasY(ty);
        } else {
            cx = tx - CANVAS_X;
            cy = ty - CANVAS_Y;
        }
        cx = constrain(cx, 0, CANVAS_W - 1);
        cy = constrain(cy, 0, CANVAS_H - 1);

        if (activeTool == TOOL_SEL_RECT) {
            if (selPhase == SEL_NONE || selPhase == SEL_CORNER1) {
                selX1 = cx; selY1 = cy;
                selPhase = SEL_CORNER2;
                // Draw crosshair for corner 1
                int ox, oy;
                if ((zoomLevel > 0)) {
                    ox = CANVAS_X + (cx - zoomVX) * CANVAS_W / zoomVW;
                    oy = CANVAS_Y + (cy - zoomVY) * CANVAS_H / zoomVH;
                } else { ox = CANVAS_X + cx; oy = CANVAS_Y + cy; }
                tft.drawLine(ox-5, oy, ox+5, oy, C_CYAN);
                tft.drawLine(ox, oy-5, ox, oy+5, C_CYAN);
                setStatus("Tap corner 2");
            } else if (selPhase == SEL_CORNER2) {
                selX2 = cx; selY2 = cy;
                // Normalize
                if (selX1 > selX2) { int t=selX1; selX1=selX2; selX2=t; }
                if (selY1 > selY2) { int t=selY1; selY1=selY2; selY2=t; }
                undoPush();  // snapshot before cut
                selCaptureRect();
                selPhase = SEL_PLACE;
                pushCanvas();  // show the cleared source area
                selDrawOutline();
                setStatus("Tap dest (inside=keep)");
            } else if (selPhase == SEL_PLACE) {
                if (cx >= selX1 && cx <= selX2 && cy >= selY1 && cy <= selY2) {
                    selPlace(selX1, selY1);
                } else {
                    selPlace(cx, cy);
                }
                pushCanvas();
                drawBottomBar();
                drawStatusBar();
            }
        } else if (activeTool == TOOL_LASSO) {
            if (selPhase == SEL_NONE) {
                // First tap starts lasso drawing
                selPhase = SEL_LASSO_DRAW;
                lassoPtCount = 0;
                lassoX[0] = cx; lassoY[0] = cy;
                lassoPtCount = 1;
                setStatus("Draw outline...");
            } else if (selPhase == SEL_LASSO_PLACE) {
                if (cx >= selX1 && cx <= selX2 && cy >= selY1 && cy <= selY2) {
                    selPlace(selX1, selY1);
                } else {
                    selPlace(cx, cy);
                }
                pushCanvas();
                drawBottomBar();
                drawStatusBar();
            }
        }
        return;
    }

    // ── Single-tap canvas tools (Fill / Eyedropper) ──────────────────────────
    // These need their own debounce, completely independent of the UI debounce,
    // so a tool-select tap doesn't block the very next canvas tap.
    if (currentScreen == SCR_MAIN && !zoomPending &&
        rectHit(tx, ty, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H) &&
        isSingleTapTool(activeTool))
    {
        lastDrawX  = -1;
        lastDrawY  = -1;
        wasDrawing = false;

        unsigned long now = millis();
        if (now - lastCanvasTap < CANVAS_DEBOUNCE_MS) return;
        lastCanvasTap = now;

        int cx, cy;
        if ((zoomLevel > 0)) {
            cx = screenToCanvasX(tx);
            cy = screenToCanvasY(ty);
        } else {
            cx = tx - CANVAS_X;
            cy = ty - CANVAS_Y;
        }
        cx = constrain(cx, 0, CANVAS_W - 1);
        cy = constrain(cy, 0, CANVAS_H - 1);

        if (activeTool == TOOL_FILL) {
            undoPush();
            floodFill(cx, cy, activeColor);
            pushCanvas();
        } else if (activeTool == TOOL_EYEDROP) {
            activeColor = canvasGet(cx, cy);
            drawBottomBar();
        } else if (activeTool == TOOL_COL_REPLACE) {
            // v24: Replace tapped color with active color
            uint16_t oldC = canvasGet(cx, cy);
            if (oldC != activeColor) {
                undoPush();
                for (int p = 0; p < CANVAS_W * CANVAS_H; p++) {
                    if (colorMatch565(canvas[p], oldC)) canvas[p] = activeColor;
                }
                pushCanvas();
                setStatus("Colors replaced");
            }
        } else if (activeTool == TOOL_WAND) {
            // v24: Magic wand selection
            undoPush();
            wandSelect(cx, cy);
        } else if (activeTool == TOOL_TEXT) {
            // Store placement position, open text input screen
            shapeOriginX = cx; shapeOriginY = cy;
            drawTextInputScreen();
        }
        return;
    }

    // ── Two-tap shape tools (also use canvas debounce) ───────────────────────
    if (currentScreen == SCR_MAIN && !zoomPending &&
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


    // ── Multi-tap tools (polygon, curve) ──────────────────────────────────
    if (currentScreen == SCR_MAIN && !zoomPending &&
        rectHit(tx, ty, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H) &&
        isMultiTapTool(activeTool))
    {
        lastDrawX = -1; lastDrawY = -1; wasDrawing = false;
        unsigned long nowMT = millis();
        if (nowMT - lastCanvasTap < CANVAS_DEBOUNCE_MS) return;

        int cx, cy;
        if (zoomLevel > 0) { cx = screenToCanvasX(tx); cy = screenToCanvasY(ty); }
        else { cx = tx - CANVAS_X; cy = ty - CANVAS_Y; }
        cx = constrain(cx, 0, CANVAS_W - 1);
        cy = constrain(cy, 0, CANVAS_H - 1);

        if (activeTool == TOOL_POLYGON) {
            // Double-tap detection: close polygon if tapping near last point quickly
            if (polyPtCount >= 3 && (nowMT - polyLastTap < 450) &&
                abs(cx - polyX[polyPtCount-1]) < 8 && abs(cy - polyY[polyPtCount-1]) < 8) {
                undoPush();
                drawPolygon();
                pushCanvas();
                polyPtCount = 0;
                setStatus("Polygon done");
                lastCanvasTap = nowMT;
                return;
            }
            polyLastTap = nowMT;
            if (polyPtCount < POLY_MAX_PTS) {
                polyX[polyPtCount] = cx;
                polyY[polyPtCount] = cy;
                polyPtCount++;
                // Draw vertex marker on screen
                int sx2, sy2;
                if (zoomLevel > 0) {
                    sx2 = CANVAS_X + (cx - zoomVX) * CANVAS_W / zoomVW;
                    sy2 = CANVAS_Y + (cy - zoomVY) * CANVAS_H / zoomVH;
                } else { sx2 = CANVAS_X + cx; sy2 = CANVAS_Y + cy; }
                tft.fillCircle(sx2, sy2, 3, C_LIME);
                // Draw edge to previous point
                if (polyPtCount > 1) {
                    int px = polyX[polyPtCount-2], py = polyY[polyPtCount-2];
                    int psx, psy;
                    if (zoomLevel > 0) {
                        psx = CANVAS_X + (px - zoomVX) * CANVAS_W / zoomVW;
                        psy = CANVAS_Y + (py - zoomVY) * CANVAS_H / zoomVH;
                    } else { psx = CANVAS_X + px; psy = CANVAS_Y + py; }
                    tft.drawLine(psx, psy, sx2, sy2, C_LIME);
                }
                char pmsg[16]; snprintf(pmsg, 16, "Pt %d (x2=end)", polyPtCount);
                setStatus(pmsg);
            }
        } else if (activeTool == TOOL_CURVE) {
            if (curvePhase == 0) {
                curveX0 = cx; curveY0 = cy;
                curvePhase = 1;
                int sx2, sy2;
                if (zoomLevel > 0) { sx2 = CANVAS_X+(cx-zoomVX)*CANVAS_W/zoomVW; sy2 = CANVAS_Y+(cy-zoomVY)*CANVAS_H/zoomVH; }
                else { sx2 = CANVAS_X+cx; sy2 = CANVAS_Y+cy; }
                tft.drawLine(sx2-5, sy2, sx2+5, sy2, C_CYAN);
                tft.drawLine(sx2, sy2-5, sx2, sy2+5, C_CYAN);
                setStatus("Tap end point");
            } else if (curvePhase == 1) {
                curveX1 = cx; curveY1 = cy;
                curvePhase = 2;
                int sx2, sy2;
                if (zoomLevel > 0) { sx2 = CANVAS_X+(cx-zoomVX)*CANVAS_W/zoomVW; sy2 = CANVAS_Y+(cy-zoomVY)*CANVAS_H/zoomVH; }
                else { sx2 = CANVAS_X+cx; sy2 = CANVAS_Y+cy; }
                tft.drawLine(sx2-5, sy2, sx2+5, sy2, C_MAGENTA);
                tft.drawLine(sx2, sy2-5, sx2, sy2+5, C_MAGENTA);
                setStatus("Tap control pt");
            } else {
                undoPush();
                drawBezierCurve(curveX0, curveY0, curveX1, curveY1, cx, cy);
                pushCanvas();
                curvePhase = 0;
                setStatus("Curve drawn");
            }
        }
        lastCanvasTap = nowMT;
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
        case SCR_ANIM:       handleAnimTouch(tx, ty); break;
        case SCR_RENAME:     handleRenameTouch(tx, ty); break;
        case SCR_TOOL_SEL:   handleToolSelectTouch(tx, ty); break;
        case SCR_BRUSH_SEL:  handleBrushSelectTouch(tx, ty); break;
        case SCR_TEXT_INPUT:  handleTextInputTouch(tx, ty); break;
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
    if ((zoomLevel > 0)) {
        pushCanvasZoomed(); return;
    }
    tft.startWrite();
    tft.setAddrWindow(CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H);
    tft.pushPixels(canvas, CANVAS_W * CANVAS_H);
    tft.endWrite();
}

// ─────────────────────────────────────────────────────────────────────────────
// Zoom helpers
// ─────────────────────────────────────────────────────────────────────────────

// Convert screen pixel to canvas coordinate (accounts for zoom viewport)
int screenToCanvasX(int sx) {
    return zoomVX + (sx - CANVAS_X) * zoomVW / CANVAS_W;
}
int screenToCanvasY(int sy) {
    return zoomVY + (sy - CANVAS_Y) * zoomVH / CANVAS_H;
}

void zoomIn(int cx, int cy) {
    // cx,cy are in canvas space; zoom in centred on that point
    if (zoomLevel >= ZOOM_MAX_LEVEL) { zoomPending = false; return; }
    zoomLevel++;
    zoomPending = false;
    // New viewport is half the current viewport size, centred on (cx,cy)
    int newW = CANVAS_W >> zoomLevel;  // level 1→100, level 2→50
    int newH = CANVAS_H >> zoomLevel;
    zoomVX = constrain(cx - newW / 2, 0, CANVAS_W - newW);
    zoomVY = constrain(cy - newH / 2, 0, CANVAS_H - newH);
    zoomVW = newW;
    zoomVH = newH;
}

void zoomOut() {
    zoomPending = false;
    if (zoomLevel <= 0) return;
    // Remember the centre of the current viewport so we can re-centre the wider view
    int cx = zoomVX + zoomVW / 2;
    int cy = zoomVY + zoomVH / 2;
    zoomLevel--;
    if (zoomLevel == 0) {
        zoomVX = 0; zoomVY = 0;
        zoomVW = CANVAS_W; zoomVH = CANVAS_H;
    } else {
        int newW = CANVAS_W >> zoomLevel;
        int newH = CANVAS_H >> zoomLevel;
        zoomVX = constrain(cx - newW / 2, 0, CANVAS_W - newW);
        zoomVY = constrain(cy - newH / 2, 0, CANVAS_H - newH);
        zoomVW = newW;
        zoomVH = newH;
    }
}

// Push canvas to TFT with zoom (nearest-neighbour scaling)
void pushCanvasZoomed() {
    static uint16_t lb[CANVAS_W];
    tft.startWrite();
    tft.setAddrWindow(CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H);
    for (int sy = 0; sy < CANVAS_H; sy++) {
        int srcY = zoomVY + sy * zoomVH / CANVAS_H;
        if (srcY >= CANVAS_H) srcY = CANVAS_H - 1;
        for (int sx = 0; sx < CANVAS_W; sx++) {
            int srcX = zoomVX + sx * zoomVW / CANVAS_W;
            if (srcX >= CANVAS_W) srcX = CANVAS_W - 1;
            lb[sx] = canvas[srcY * CANVAS_W + srcX];
        }
        // Grid overlay at zoom >= 2 (4x and above only)
// Grid overlay aligned to actual viewport sampling (fixes 16x drift)
if (gridOverlay && zoomLevel >= 2) {
    static int prevSrcY = -1;
    bool newRow = (srcY != prevSrcY);
    prevSrcY = srcY;

    int prevSrcX = -1;
    for (int sx = 0; sx < CANVAS_W; sx++) {
        int srcX = zoomVX + sx * zoomVW / CANVAS_W;

        bool newCol = (srcX != prevSrcX);
        prevSrcX = srcX;

        if (newCol || newRow) {
            // XOR with gray for visibility on any background
            lb[sx] ^= 0x4208;
        }
    }
}
        tft.pushPixels(lb, CANVAS_W);
    }
    tft.endWrite();
}

// ─────────────────────────────────────────────────────────────────────────────
// Undo / Redo
// ─────────────────────────────────────────────────────────────────────────────

static void undoFreeSlot(UndoSlot *s) {
    if (s->data) { free(s->data); s->data = nullptr; s->size = 0; }
}

void undoClearAll() {
    for (int i = 0; i < UNDO_MAX_SLOTS; i++) undoFreeSlot(&undoStack[i]);
    undoCount = 0; undoTop = 0;
}

void redoClearAll() {
    for (int i = 0; i < UNDO_MAX_SLOTS; i++) undoFreeSlot(&redoStack[i]);
    redoCount = 0; redoTop = 0;
}

// Push current canvas state onto undo stack (call BEFORE modifying canvas)
void undoPush() {
    // Dry-run to get compressed size
    uint32_t sz = rleCompress(canvas, nullptr, 0);
    if (sz == 0) return;
    // Check heap budget
    if (ESP.getFreeHeap() - sz < UNDO_HEAP_RESERVE) {
        // Drop oldest undo to make room
        if (undoCount > 0) {
            int oldest = (undoTop - undoCount + UNDO_MAX_SLOTS) % UNDO_MAX_SLOTS;
            undoFreeSlot(&undoStack[oldest]);
            undoCount--;
        }
        if (ESP.getFreeHeap() - sz < UNDO_HEAP_RESERVE) return;  // still not enough
    }
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) return;
    rleCompress(canvas, buf, sz);
    // Free the slot we're about to overwrite
    undoFreeSlot(&undoStack[undoTop]);
    undoStack[undoTop].data = buf;
    undoStack[undoTop].size = sz;
    undoTop = (undoTop + 1) % UNDO_MAX_SLOTS;
    if (undoCount < UNDO_MAX_SLOTS) undoCount++;
    // Any new action invalidates the redo stack
    redoClearAll();
}

// Undo: restore previous canvas state
void undoPerform() {
    if (undoCount <= 0) return;
    // Push current state to redo stack before restoring
    uint32_t rsz = rleCompress(canvas, nullptr, 0);
    if (rsz > 0 && ESP.getFreeHeap() - rsz >= UNDO_HEAP_RESERVE) {
        uint8_t *rbuf = (uint8_t *)malloc(rsz);
        if (rbuf) {
            rleCompress(canvas, rbuf, rsz);
            undoFreeSlot(&redoStack[redoTop]);
            redoStack[redoTop].data = rbuf;
            redoStack[redoTop].size = rsz;
            redoTop = (redoTop + 1) % UNDO_MAX_SLOTS;
            if (redoCount < UNDO_MAX_SLOTS) redoCount++;
        }
    }
    // Pop from undo stack
    undoTop = (undoTop - 1 + UNDO_MAX_SLOTS) % UNDO_MAX_SLOTS;
    undoCount--;
    rleDecompress(undoStack[undoTop].data, undoStack[undoTop].size, canvas);
    undoFreeSlot(&undoStack[undoTop]);
    pushCanvas();
    if (currentScreen == SCR_MAIN) drawStatusBar();
}

// Redo: restore next canvas state
void redoPerform() {
    if (redoCount <= 0) return;
    // Push current state back to undo stack
    uint32_t usz = rleCompress(canvas, nullptr, 0);
    if (usz > 0) {
        uint8_t *ubuf = (uint8_t *)malloc(usz);
        if (ubuf) {
            rleCompress(canvas, ubuf, usz);
            undoFreeSlot(&undoStack[undoTop]);
            undoStack[undoTop].data = ubuf;
            undoStack[undoTop].size = usz;
            undoTop = (undoTop + 1) % UNDO_MAX_SLOTS;
            if (undoCount < UNDO_MAX_SLOTS) undoCount++;
        }
    }
    // Pop from redo stack
    redoTop = (redoTop - 1 + UNDO_MAX_SLOTS) % UNDO_MAX_SLOTS;
    redoCount--;
    rleDecompress(redoStack[redoTop].data, redoStack[redoTop].size, canvas);
    undoFreeSlot(&redoStack[redoTop]);
    pushCanvas();
    if (currentScreen == SCR_MAIN) drawStatusBar();
}

// ─────────────────────────────────────────────────────────────────────────────
// Selection tools
// ─────────────────────────────────────────────────────────────────────────────

void selCancel() {
    selPhase = SEL_NONE;
    lassoPtCount = 0;
    if (selBuf) { free(selBuf); selBuf = nullptr; }
    if (selMask) { free(selMask); selMask = nullptr; }
    selW = 0; selH = 0;
}

// Get a bit from the lasso mask (packed 8 pixels per byte)
bool selMaskGet(int x, int y) {
    if (!selMask || x < 0 || x >= selW || y < 0 || y >= selH) return false;
    int bit = y * selW + x;
    return (selMask[bit >> 3] >> (bit & 7)) & 1;
}

// Set a bit in the lasso mask
static void selMaskSet(uint8_t *mask, int w, int x, int y) {
    int bit = y * w + x;
    mask[bit >> 3] |= (1 << (bit & 7));
}

// Capture rectangular selection from canvas
void selCaptureRect() {
    selW = selX2 - selX1 + 1;
    selH = selY2 - selY1 + 1;
    if (selW <= 0 || selH <= 0) { selCancel(); return; }


    // If an older selection buffer is still around, free it (safe no-op).
    if (selBuf) { free(selBuf); selBuf = nullptr; }
    if (selMask) { free(selMask); selMask = nullptr; }
    size_t sz = selW * selH * sizeof(uint16_t);
    selBuf = (uint16_t *)malloc(sz);
    if (!selBuf) { setStatus("No mem for sel"); selCancel(); return; }


    // v24 upgrade: always keep a 1-bit mask for selections.
    // Rect selections are fully opaque, so all bits are 1.
    int maskBytes = (selW * selH + 7) / 8;
    selMask = (uint8_t *)malloc(maskBytes);
    if (!selMask) { setStatus("No mem for mask"); selCancel(); return; }
    memset(selMask, 0xFF, maskBytes);
    // Copy pixels from canvas
    for (int y = 0; y < selH; y++) {
        for (int x = 0; x < selW; x++) {
            selBuf[y * selW + x] = canvas[(selY1 + y) * CANVAS_W + (selX1 + x)];
        }
    }
    // v24: Clear source only in cut mode
    if (selCutMode) {
        for (int y = selY1; y <= selY2; y++)
            for (int x = selX1; x <= selX2; x++)
                canvas[y * CANVAS_W + x] = C_WHITE;
    }
}

// Build the lasso mask from the outline points using scanline fill
void selBuildLassoMask() {
    if (lassoPtCount < 3) return;

    // Find bounding box
    int minX = CANVAS_W, minY = CANVAS_H, maxX = 0, maxY = 0;
    for (int i = 0; i < lassoPtCount; i++) {
        if (lassoX[i] < minX) minX = lassoX[i];
        if (lassoY[i] < minY) minY = lassoY[i];
        if (lassoX[i] > maxX) maxX = lassoX[i];
        if (lassoY[i] > maxY) maxY = lassoY[i];
    }
    selX1 = minX; selY1 = minY; selX2 = maxX; selY2 = maxY;
    selW = selX2 - selX1 + 1;
    selH = selY2 - selY1 + 1;
    if (selW <= 0 || selH <= 0) return;

    // Allocate mask (1 bit per pixel, packed)
    int maskBytes = (selW * selH + 7) / 8;
    selMask = (uint8_t *)calloc(maskBytes, 1);
    if (!selMask) { setStatus("No mem for mask"); return; }

    // First: mark all edge pixels on the lasso outline
    for (int i = 0; i < lassoPtCount; i++) {
        int j = (i + 1) % lassoPtCount;
        // Bresenham line between consecutive lasso points
        int ax = lassoX[i] - selX1, ay = lassoY[i] - selY1;
        int bx = lassoX[j] - selX1, by = lassoY[j] - selY1;
        int dx = abs(bx - ax), sx = ax < bx ? 1 : -1;
        int dy = -abs(by - ay), sy = ay < by ? 1 : -1;
        int err = dx + dy;
        while (true) {
            if (ax >= 0 && ax < selW && ay >= 0 && ay < selH)
                selMaskSet(selMask, selW, ax, ay);
            if (ax == bx && ay == by) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; ax += sx; }
            if (e2 <= dx) { err += dx; ay += sy; }
        }
    }

    // Scanline fill: for each row, use ray-casting to determine inside/outside
    for (int row = 0; row < selH; row++) {
        // Count edge crossings from left using the polygon edges
        // Simple even-odd rule scanline
        int canvasRow = selY1 + row;
        for (int col = 0; col < selW; col++) {
            int canvasCol = selX1 + col;
            // Ray-cast from this point to the left — count polygon edge crossings
            int crossings = 0;
            for (int i = 0; i < lassoPtCount; i++) {
                int j = (i + 1) % lassoPtCount;
                int yi = lassoY[i], yj = lassoY[j];
                int xi = lassoX[i], xj = lassoX[j];
                // Does this edge cross the horizontal ray at canvasRow going left from canvasCol?
                if ((yi <= canvasRow && yj > canvasRow) || (yj <= canvasRow && yi > canvasRow)) {
                    float xIntersect = xi + (float)(canvasRow - yi) / (yj - yi) * (xj - xi);
                    if (xIntersect < canvasCol) crossings++;
                }
            }
            if (crossings & 1) {
                selMaskSet(selMask, selW, col, row);
            }
        }
    }
}

// Capture lasso selection from canvas using mask
void selCaptureLasso() {
    
    // Free any existing selection buffers (safe no-op).
    if (selBuf) { free(selBuf); selBuf = nullptr; }
    if (selMask) { free(selMask); selMask = nullptr; }
selBuildLassoMask();
    if (!selMask || selW <= 0 || selH <= 0) { selCancel(); return; }

    size_t sz = selW * selH * sizeof(uint16_t);
    selBuf = (uint16_t *)malloc(sz);
    if (!selBuf) { setStatus("No mem for sel"); selCancel(); return; }

    // Copy masked pixels, clear source
    for (int y = 0; y < selH; y++) {
        for (int x = 0; x < selW; x++) {
            if (selMaskGet(x, y)) {
                selBuf[y * selW + x] = canvas[(selY1 + y) * CANVAS_W + (selX1 + x)];
                if (selCutMode) canvas[(selY1 + y) * CANVAS_W + (selX1 + x)] = C_WHITE;
            } else {
                selBuf[y * selW + x] = C_WHITE;  // transparent
            }
        }
    }
}

// Place selection at destination
void selPlace(int destX, int destY) {
    if (!selBuf || selW <= 0 || selH <= 0) { selCancel(); return; }

    for (int y = 0; y < selH; y++) {
        for (int x = 0; x < selW; x++) {
            int dx = destX + x, dy = destY + y;
            if (dx < 0 || dx >= CANVAS_W || dy < 0 || dy >= CANVAS_H) continue;
            bool masked = selMask ? selMaskGet(x, y) : true;
            if (masked) {
                canvas[dy * CANVAS_W + dx] = selBuf[y * selW + x];
            }
        }
    }
    selCancel();
    setStatus("Placed");
}

// Draw dashed outline of selection on TFT (overlay, doesn't modify canvas)
void selDrawOutline() {
    if (selW <= 0 || selH <= 0) return;
    // Map selection bounds to screen coords
    int sx1, sy1, sx2, sy2;
    if ((zoomLevel > 0)) {
        sx1 = CANVAS_X + (selX1 - zoomVX) * CANVAS_W / zoomVW;
        sy1 = CANVAS_Y + (selY1 - zoomVY) * CANVAS_H / zoomVH;
        sx2 = CANVAS_X + (selX2 - zoomVX) * CANVAS_W / zoomVW;
        sy2 = CANVAS_Y + (selY2 - zoomVY) * CANVAS_H / zoomVH;
    } else {
        sx1 = CANVAS_X + selX1; sy1 = CANVAS_Y + selY1;
        sx2 = CANVAS_X + selX2; sy2 = CANVAS_Y + selY2;
    }
    // Draw dashed rect
    for (int x = sx1; x <= sx2; x++) {
        if ((x & 3) < 2) { tft.drawPixel(x, sy1, C_CYAN); tft.drawPixel(x, sy2, C_CYAN); }
    }
    for (int y = sy1; y <= sy2; y++) {
        if ((y & 3) < 2) { tft.drawPixel(sx1, y, C_CYAN); tft.drawPixel(sx2, y, C_CYAN); }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// v24: Helper functions
// ─────────────────────────────────────────────────────────────────────────────

// Color distance for tolerance
int colorDist565(uint16_t a, uint16_t b) {
    int dR = abs(((a >> 11) & 0x1F) - ((b >> 11) & 0x1F));
    int dG = abs(((a >> 5) & 0x3F)  - ((b >> 5) & 0x3F));
    int dB = abs((a & 0x1F) - (b & 0x1F));
    return dR + dG + dB;
}

bool colorMatch(uint16_t a, uint16_t b) {
    if (fillTolerance == 0) return a == b;
    return colorDist565(a, b) <= (int)fillTolerance;
}

// v24: colorMatch565 for color replace (always uses tolerance)
bool colorMatch565(uint16_t a, uint16_t b) {
    return colorMatch(a, b);
}

bool patternCheck(int x, int y) {
    switch (patternMode) {
        case 0: return true;
        case 1: return ((x + y) & 1) == 0;
        case 2: return (x % 3 == 0) && (y % 3 == 0);
        case 3: return (y % 3 == 0);
        case 4: return (x % 3 == 0);
        case 5: return ((x + y) % 4 == 0);
        default: return true;
    }
}



void selFlipH() {
    if (!selBuf || selW <= 0 || selH <= 0) return;
    for (int y = 0; y < selH; y++) {
        for (int x = 0; x < selW / 2; x++) {
            int mx = selW - 1 - x;
            uint16_t tmp = selBuf[y * selW + x];
            selBuf[y * selW + x] = selBuf[y * selW + mx];
            selBuf[y * selW + mx] = tmp;
            if (selMask) {
                bool a = selMaskGet(x, y), b = selMaskGet(mx, y);
                int bitA = y * selW + x, bitB = y * selW + mx;
                if (a) selMask[bitB >> 3] |= (1 << (bitB & 7)); else selMask[bitB >> 3] &= ~(1 << (bitB & 7));
                if (b) selMask[bitA >> 3] |= (1 << (bitA & 7)); else selMask[bitA >> 3] &= ~(1 << (bitA & 7));
            }
        }
    }
    setStatus("Flipped H");
}

void selFlipV() {
    if (!selBuf || selW <= 0 || selH <= 0) return;
    for (int y = 0; y < selH / 2; y++) {
        int my = selH - 1 - y;
        for (int x = 0; x < selW; x++) {
            uint16_t tmp = selBuf[y * selW + x];
            selBuf[y * selW + x] = selBuf[my * selW + x];
            selBuf[my * selW + x] = tmp;
            if (selMask) {
                bool a = selMaskGet(x, y), b = selMaskGet(x, my);
                int bitA = y * selW + x, bitB = my * selW + x;
                if (a) selMask[bitB >> 3] |= (1 << (bitB & 7)); else selMask[bitB >> 3] &= ~(1 << (bitB & 7));
                if (b) selMask[bitA >> 3] |= (1 << (bitA & 7)); else selMask[bitA >> 3] &= ~(1 << (bitA & 7));
            }
        }
    }
    setStatus("Flipped V");
}



void selRotate90() {
    if (!selBuf || selW <= 0 || selH <= 0) return;

    // Rotate selection 90° clockwise (CW). This keeps top-left anchored.
    int newW = selH;
    int newH = selW;

    size_t pixBytes = (size_t)newW * (size_t)newH * sizeof(uint16_t);
    uint16_t *newBuf = (uint16_t *)malloc(pixBytes);
    if (!newBuf) { setStatus("No mem rot"); return; }

    uint8_t *newMask = nullptr;
    if (selMask) {
        int maskBytes = (newW * newH + 7) / 8;
        newMask = (uint8_t *)calloc(maskBytes, 1);
        if (!newMask) { free(newBuf); setStatus("No mem rot"); return; }
    }

    for (int y = 0; y < selH; y++) {
        for (int x = 0; x < selW; x++) {
            int nx = (selH - 1 - y);
            int ny = x;
            newBuf[ny * newW + nx] = selBuf[y * selW + x];
            if (selMask && selMaskGet(x, y)) {
                selMaskSet(newMask, newW, nx, ny);
            }
        }
    }

    free(selBuf);
    selBuf = newBuf;
    if (selMask) {
        free(selMask);
        selMask = newMask;
    }

    selW = newW;
    selH = newH;
    selX2 = selX1 + selW - 1;
    selY2 = selY1 + selH - 1;

    setStatus("Rot 90");
}

// v24: Magic wand — flood-fill to build a selection
void wandSelect(int x, int y) {
    if (x < 0 || x >= CANVAS_W || y < 0 || y >= CANVAS_H) return;

    // Free any existing selection buffers before creating a new wand selection.
    if (selBuf) { free(selBuf); selBuf = nullptr; }
    if (selMask) { free(selMask); selMask = nullptr; }
    lassoPtCount = 0;
    uint16_t target = canvasGet(x, y);
    int maskBytes = (CANVAS_W * CANVAS_H + 7) / 8;
    uint8_t *vis = (uint8_t *)calloc(maskBytes, 1);
    if (!vis) { setStatus("No mem for wand"); return; }

    struct Span { int16_t x1, x2, row, dy; };
    static Span wq[800];
    int head = 0, tail = 0;
    #define WPU(a,b,r,d) { wq[tail]={int16_t(a),int16_t(b),int16_t(r),int16_t(d)}; tail=(tail+1)%800; }
    #define WPO(s) { s=wq[head]; head=(head+1)%800; }
    #define WEM (head==tail)
    #define VS(px,py) { int _b=(py)*CANVAS_W+(px); vis[_b>>3]|=(1<<(_b&7)); }
    #define VG(px,py) ((vis[((py)*CANVAS_W+(px))>>3]>>(((py)*CANVAS_W+(px))&7))&1)

    int lx=x, rx=x;
    while(lx>0 && colorMatch(canvasGet(lx-1,y),target) && !VG(lx-1,y)) lx--;
    while(rx<CANVAS_W-1 && colorMatch(canvasGet(rx+1,y),target) && !VG(rx+1,y)) rx++;
    for(int i=lx;i<=rx;i++) VS(i,y);
    if(y>0) WPU(lx,rx,y-1,-1) if(y<CANVAS_H-1) WPU(lx,rx,y+1,+1)
    int minX=lx,maxX=rx,minY=y,maxY=y;

    while(!WEM) {
        Span s; WPO(s)
        int i=s.x1;
        while(i<=s.x2) {
            if(!colorMatch(canvasGet(i,s.row),target)||VG(i,s.row)){i++;continue;}
            int sL=i;
            while(i<=s.x2 && colorMatch(canvasGet(i,s.row),target) && !VG(i,s.row)){VS(i,s.row);i++;}
            int sR=i-1, xl=sL, xr=sR;
            while(xl>0 && colorMatch(canvasGet(xl-1,s.row),target) && !VG(xl-1,s.row)){xl--;VS(xl,s.row);}
            while(xr<CANVAS_W-1 && colorMatch(canvasGet(xr+1,s.row),target) && !VG(xr+1,s.row)){xr++;VS(xr,s.row);}
            if(xl<minX)minX=xl; if(xr>maxX)maxX=xr;
            if(s.row<minY)minY=s.row; if(s.row>maxY)maxY=s.row;
            int nr=s.row+s.dy; if(nr>=0&&nr<CANVAS_H) WPU(xl,xr,nr,s.dy)
            int pr=s.row-s.dy; if(pr>=0&&pr<CANVAS_H&&(xl<s.x1||xr>s.x2)) WPU(xl,xr,pr,-s.dy)
        }
    }
    #undef WPU
    #undef WPO
    #undef WEM

    selX1=minX; selY1=minY; selX2=maxX; selY2=maxY;
    selW=selX2-selX1+1; selH=selY2-selY1+1;
    if(selW<=0||selH<=0){free(vis);selCancel();return;}

    selBuf=(uint16_t*)malloc(selW*selH*sizeof(uint16_t));
    int smb=(selW*selH+7)/8;
    selMask=(uint8_t*)calloc(smb,1);
    if(!selBuf||!selMask){free(vis);setStatus("No mem wand");selCancel();return;}

    for(int row=0;row<selH;row++){
        for(int col=0;col<selW;col++){
            int cx2=selX1+col, cy2=selY1+row;
            if(VG(cx2,cy2)){
                selBuf[row*selW+col]=canvas[cy2*CANVAS_W+cx2];
                int bit=row*selW+col; selMask[bit>>3]|=(1<<(bit&7));
                if(selCutMode) canvas[cy2*CANVAS_W+cx2]=C_WHITE;
            } else {
                selBuf[row*selW+col]=C_WHITE;
            }
        }
    }
    free(vis);
    #undef VS
    #undef VG
    selPhase=SEL_PLACE;
    pushCanvas();
    selDrawOutline();
    setStatus(selCutMode?"Wand cut: tap dest/keep":"Wand copy: tap dest/keep");
}



// v24: Native .CYD project format save
// Header: "CYD1" (4) + width:u16 + height:u16 + frameCount:u8 + fps:u8
//         + frameDelay[MAX_FRAMES]:u16 (16 bytes) + frameSizes[MAX_FRAMES]:u32 (32 bytes)
// Then concatenated RLE blobs
void saveCYD() {
    if (!sdOK) return;
    // Compress live frame
    animCompressAndStore(animCurFrame);

    char path[20]; int n = 0;
    for (; n < 1000; n++) {
        snprintf(path, 20, "/PRJ%03d.CYD", n);
        if (!SD.exists(path)) break;
    }
    File f = SD.open(path, FILE_WRITE);
    if (!f) { setStatus("Save CYD fail"); return; }

    // Header
    f.write((uint8_t*)"CYD1", 4);
    uint16_t w = CANVAS_W, h = CANVAS_H;
    f.write((uint8_t*)&w, 2);
    f.write((uint8_t*)&h, 2);
    uint8_t fc = animFrameCount, fps = animFPS;
    f.write(&fc, 1);
    f.write(&fps, 1);
    // Per-frame delays
    for (int i = 0; i < MAX_FRAMES; i++) {
        f.write((uint8_t*)&animFrameDelay[i], 2);
    }
    // Per-frame RLE sizes
    uint32_t sizes[MAX_FRAMES];
    for (int i = 0; i < MAX_FRAMES; i++) {
        sizes[i] = (i < animFrameCount && animStore[i].data) ? animStore[i].size : 0;
    }
    f.write((uint8_t*)sizes, sizeof(sizes));

    // RLE data
    for (int i = 0; i < animFrameCount; i++) {
        if (animStore[i].data && animStore[i].size > 0) {
            f.write(animStore[i].data, animStore[i].size);
        }
    }
    f.close();

    // Restore live frame
    animLoadFromStore(animCurFrame);
    if (animStore[animCurFrame].data) { free(animStore[animCurFrame].data); animStore[animCurFrame].data = nullptr; animStore[animCurFrame].size = 0; }

    countFiles(); updateStatusCounts();
    setStatus("CYD saved OK");
}

// v24: Load a .CYD project file
void loadCYD(int idx) {
    if (!sdOK || idx < 0 || idx >= galleryCount) return;
    char path[20];
    snprintf(path, 20, "/%s", galleryFiles[idx]);
    File f = SD.open(path);
    if (!f) { setStatus("Can't open CYD"); return; }

    // Read header
    uint8_t hdr[58];
    if (f.read(hdr, 58) != 58 || hdr[0]!='C' || hdr[1]!='Y' || hdr[2]!='D' || hdr[3]!='1') {
        f.close(); setStatus("Bad CYD file"); return;
    }
    uint16_t w = hdr[4] | (hdr[5]<<8);
    uint16_t h = hdr[6] | (hdr[7]<<8);
    if (w != CANVAS_W || h != CANVAS_H) {
        f.close(); setStatus("Wrong CYD size"); return;
    }
    uint8_t fc = hdr[8], fps = hdr[9];
    if (fc < 1 || fc > MAX_FRAMES) fc = 1;

    // Per-frame delays
    for (int i = 0; i < MAX_FRAMES; i++) {
        animFrameDelay[i] = hdr[10 + i*2] | (hdr[11 + i*2]<<8);
    }
    // Per-frame RLE sizes
    uint32_t sizes[MAX_FRAMES];
    for (int i = 0; i < MAX_FRAMES; i++) {
        int off = 26 + i*4;
        sizes[i] = hdr[off] | (hdr[off+1]<<8) | (hdr[off+2]<<16) | (hdr[off+3]<<24);
    }

    // Clear current animation
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (animStore[i].data) { free(animStore[i].data); animStore[i].data = nullptr; animStore[i].size = 0; }
    }
    undoClearAll(); redoClearAll();

    animFrameCount = 0;
    animFPS = fps;
    // Read RLE blobs
    for (int i = 0; i < fc; i++) {
        if (sizes[i] == 0 || sizes[i] > ANIM_MAX_RLE_BYTES) break;
        uint8_t *buf = (uint8_t *)malloc(sizes[i]);
        if (!buf) break;
        if (f.read(buf, sizes[i]) != (int)sizes[i]) { free(buf); break; }
        animStore[i].data = buf;
        animStore[i].size = sizes[i];
        animFrameCount++;
    }
    f.close();

    if (animFrameCount == 0) {
        clearCanvas(C_WHITE);
        animFrameCount = 1;
        animCurFrame = 0;
        setStatus("CYD empty?");
        return;
    }
    // Load frame 0 as live
    animLoadFromStore(0);
    if (animStore[0].data) { free(animStore[0].data); animStore[0].data = nullptr; animStore[0].size = 0; }
    animCurFrame = 0;
    animStripScroll = 0;
    setStatus("CYD loaded OK");
}

// v24: Scan for .CYD project files
void scanCYDs() {
    galleryCount = 0;
    if (!sdOK) return;
    File root = SD.open("/");
    while (galleryCount < 128) {
        File f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            const char *nm = f.name();
            int len = strlen(nm);
            if (len > 4 && strcasecmp(nm + len - 4, ".cyd") == 0) {
                strncpy(galleryFiles[galleryCount], nm, 15);
                galleryFiles[galleryCount][15] = 0;
                galleryCount++;
            }
        }
        f.close();
    }
    root.close();
}

// v24: Duplicate current frame (insert copy after current)
void animDuplicateFrame() {
    if (animFrameCount >= MAX_FRAMES) {
        setStatus("Max frames!"); return;
    }
    animAddFrame();  // already duplicates current frame
    setStatus("Frame duplicated");
}

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

void drawButtonRect(int x, int y, int w, int h,
                    uint16_t bg, uint16_t fg, const char *label) {
    tft.fillRect(x, y, w, h, bg);
    tft.drawRect(x, y, w, h, fg);
    tft.setTextColor(fg, bg);
    tft.setTextSize(1);
    int tw = strlen(label) * 6;
    tft.setCursor(x + (w - tw) / 2, y + (h - 8) / 2);
    tft.print(label);
}

// Toggle button: active → accentBg/black, inactive → dim bg/gray
void drawToggleBtn(int x, int y, int w, int h,
                   bool active, uint16_t accentBg, const char *label) {
    uint16_t bg = active ? accentBg : C_BTN_BG;
    uint16_t fg = active ? C_BLACK  : C_DKGRAY;
    drawButtonRect(x, y, w, h, bg, fg, label);
}

// Enabled/disabled button: enabled → normal bg/white, disabled → dim bg/gray
void drawEnableBtn(int x, int y, int w, int h,
                   bool enabled, const char *label) {
    uint16_t bg = enabled ? C_BTN_BG    : C_BTN_BG_DK;
    uint16_t fg = enabled ? C_WHITE     : C_DKGRAY;
    drawButtonRect(x, y, w, h, bg, fg, label);
}

void drawSwatchStrip(int x, const uint16_t *colors, int count) {
    int sh = CANVAS_H / count;
    for (int i = 0; i < count; i++) {
        tft.fillRect(x, i * sh, 16, sh, colors[i]);
        tft.drawRect(x, i * sh, 16, sh, C_DKGRAY);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main screen draw
// ─────────────────────────────────────────────────────────────────────────────

void drawMainScreen() {
    tft.fillScreen(C_DKGRAY);
    drawLeftStrip();
    drawRightStrip();
    drawStatusBar();
    pushCanvas();
    drawBottomBar();
    currentScreen = SCR_MAIN;
}

// ── Status bar (top gap, y=0..19) ───────────────────────────────────────────

void drawStatusBar() {
    tft.fillRect(CANVAS_X, STATUS_Y, CANVAS_W, STATUS_H, C_DKGRAY);
    tft.setTextSize(1);
    tft.setCursor(CANVAS_X + 2, STATUS_Y + 6);
    if (statusMsg[0]) {
        tft.setTextColor(C_GREEN, C_DKGRAY);
        tft.print(statusMsg);
    } else {
        tft.setTextColor(C_GRAY, C_DKGRAY);
        tft.printf("I:%d C:%d U:%d %dKB",
                   imgCount, cydCount, undoCount, (int)(ESP.getFreeHeap() / 1024));
    }
}

void setStatus(const char *msg) {
    strncpy(statusMsg, msg, 31);
    statusMsg[31] = '\0';
    statusExpiry = millis() + 2000;
    if (currentScreen == SCR_MAIN) drawStatusBar();
}

void updateStatusCounts() {
    imgCount = 0;
    cydCount = 0;
    if (!sdOK) return;
    File root = SD.open("/");
    while (true) {
        File f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            const char *nm = f.name();
            int len = strlen(nm);
            if (len > 4 && strcasecmp(nm + len - 4, ".bmp") == 0) {
                bool isAFrame = (len == 13 && strncasecmp(nm, "ANM", 3) == 0 && nm[6] == '_');
                if (!isAFrame) imgCount++;
            }
        }
        f.close();
    }
    root.close();
}

// ── Side colour strips & mirror buttons ─────────────────────────────────────

void drawLeftStrip() {
    drawSwatchStrip(0, PRESET_COLORS, 12);
    drawToggleBtn(0, 200, 16, 20, mirrorX, C_CYAN, "MX");
}

void drawRightStrip() {
    drawSwatchStrip(224, customColors, 12);
    drawToggleBtn(224, 200, 16, 20, mirrorY, C_MAGENTA, "MY");
}

void drawCanvas() {
    pushCanvas();
}

// ── Bottom bar ──────────────────────────────────────────────────────────────
// Layout: 3 rows × 30 px each, starting at BOTTOM_Y (y=230)
//   Row 0: Tool prev/next, tool name, colour swatches, brush size  (primary drawing)
//   Row 1: Undo/redo, zoom, grid, sel-transforms / gradient, anim  (active canvas)
//   Row 2: File ops (compact), stabilizer, pattern, cut/copy,       (settings / less used)
//          constrain, tolerance, onion skin

// Helper: look up display name for the active tool
static const char *activeToolName() {
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (toolDisplayOrder[i] == activeTool) return toolDisplayNames[i];
    }
    return "?";
}

// Row 0: [<][PEN][>] [ToolName] [PrimaryColor / SecondaryColor] [-][Size][+]
// (unchanged — already well laid out)
static void drawBottomRow0(int ry) {
    drawButtonRect(0,  ry, 30, ROW_H, C_BTN_BG, C_WHITE, "<");
    drawButtonRect(30, ry, 30, ROW_H, C_BTN_BG, C_CYAN,  "PEN");
    drawButtonRect(60, ry, 30, ROW_H, C_BTN_BG, C_WHITE, ">");

    // Tool name button (opens tool-select menu)
    const char *tn = activeToolName();
    drawButtonRect(90, ry, 50, ROW_H, C_ORANGE, C_BLACK, tn);
    tft.drawRect(90, ry, 50, ROW_H, C_WHITE);

    // Primary + secondary colour swatches
    tft.fillRect(140, ry,      36, 15, activeColor);
    tft.drawRect(140, ry,      36, 15, C_WHITE);
    tft.fillRect(140, ry + 15, 36, 15, secondColor);
    tft.drawRect(140, ry + 15, 36, 15, C_DKGRAY);

    // Brush size: [-] [value] [+]
    drawButtonRect(176, ry, 18, ROW_H, C_BTN_BG, C_WHITE, "-");
    char bsLbl[4];
    snprintf(bsLbl, sizeof(bsLbl), "%d", brushSize);
    drawButtonRect(194, ry, 22, ROW_H, C_BTN_BG_DK, C_CYAN, bsLbl);
    drawButtonRect(216, ry, 24, ROW_H, C_BTN_BG, C_WHITE, "+");
}

// Row 1: [Undo][Redo] [Z+][Z-][GRD] [FH FV R / LINEAR|RADIAL] [Anim]
static void drawBottomRow1(int ry) {
    // Undo / Redo
    drawEnableBtn(0,  ry, 28, ROW_H, undoCount > 0, "<-");
    drawEnableBtn(28, ry, 28, ROW_H, redoCount > 0, "->");

    // Zoom in — pending (yellow), active (cyan), off (default)
    uint16_t ziBg = zoomPending ? C_YELLOW : (zoomLevel > 0 ? C_CYAN : C_BTN_BG);
    uint16_t ziFg = (zoomPending || zoomLevel > 0) ? C_BLACK : C_WHITE;
    char zlbl[4];
    if (zoomLevel == 0) strcpy(zlbl, "Z+");
    else                snprintf(zlbl, sizeof(zlbl), "%dX", 1 << zoomLevel);
    drawButtonRect(56, ry, 28, ROW_H, ziBg, ziFg, zlbl);

    // Zoom out
    drawToggleBtn(84, ry, 28, ROW_H, zoomLevel > 0, C_ORANGE, "Z-");

    // Grid overlay
    drawToggleBtn(112, ry, 28, ROW_H, gridOverlay, C_LIME, "GRD");

    // Context-sensitive area (x=140..193, 54px wide):
    // When a selection is placed → show flip/rotate transforms
    // Otherwise → show gradient linear/radial toggle
    bool selActive = selBuf && (selPhase == SEL_PLACE || selPhase == SEL_LASSO_PLACE);
    if (selActive) {
        drawButtonRect(140, ry, 18, ROW_H, C_BTN_BG, C_CYAN, "FH");
        drawButtonRect(158, ry, 18, ROW_H, C_BTN_BG, C_CYAN, "FV");
        drawButtonRect(176, ry, 18, ROW_H, C_BTN_BG, C_CYAN, "R");
    } else {
        drawToggleBtn(140, ry, 54, ROW_H, gradientRadial, C_ORANGE,
                      gradientRadial ? "RADIAL" : "LINEAR");
    }

    // Animation frame count → opens anim screen
    char albl[10];
    snprintf(albl, sizeof(albl), "A:%d", animFrameCount);
    drawButtonRect(194, ry, 46, ROW_H, C_BTN_BG, C_LIME, albl);
}

// Row 2: [SV][LD][NEW][CLR] | [Stab][Pat][X/C][Constrain][Tol][OS]
static void drawBottomRow2(int ry) {
    // File ops — compact (24–26px each)
    drawButtonRect(0,  ry, 24, ROW_H, C_BTN_BG, C_WHITE,  "SV");
    drawButtonRect(24, ry, 24, ROW_H, C_BTN_BG, C_WHITE,  "LD");
    drawButtonRect(48, ry, 26, ROW_H, C_BTN_BG, C_ORANGE, "NEW");
    drawButtonRect(74, ry, 26, ROW_H, C_BTN_BG, C_RED,    "CLR");

    // Stabilizer
    char slbl[4];
    snprintf(slbl, sizeof(slbl), "S%d", stabilizerLevel);
    drawToggleBtn(100, ry, 24, ROW_H, stabilizerLevel > 0, C_CYAN, slbl);

    // Pattern brush mode
    const char *patLbl[] = { "SOL", "CHK", "DOT", "H--", "V||", "DIA" };
    drawToggleBtn(124, ry, 28, ROW_H, patternMode > 0, C_LIME, patLbl[patternMode]);

    // Cut (X) / Copy (C)
    drawButtonRect(152, ry, 16, ROW_H,
                   selCutMode ? C_CUT_BG : C_COPY_BG, C_WHITE,
                   selCutMode ? "X" : "C");

    // Constrain toggle
    const char *conLbl = (activeTool == TOOL_GRADIENT) ? "F" : "[]";
    drawToggleBtn(168, ry, 18, ROW_H, constrainMode, C_YELLOW, conLbl);

    // Fill tolerance
    char tolbl[6];
    snprintf(tolbl, sizeof(tolbl), "T:%d", fillTolerance);
    drawToggleBtn(186, ry, 28, ROW_H, fillTolerance > 0, C_YELLOW, tolbl);

    // Onion skin
    drawToggleBtn(214, ry, 26, ROW_H, onionSkin, C_MAGENTA, "OS");
}

void drawBottomBar() {
    tft.fillRect(0, BOTTOM_Y, SCREEN_W, ROW_H * 3 + 10, C_BAR_BG);
    drawBottomRow0(BOTTOM_Y);
    drawBottomRow1(BOTTOM_Y + ROW_H);
    drawBottomRow2(BOTTOM_Y + ROW_H * 2);
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
                    if (patternCheck(nx, ny)) canvasSet(nx, ny, color);
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
                        if (patternCheck(nx, ny)) canvasSet(nx, ny, color);
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
        if ((zoomLevel > 0)) {
            pushCanvasZoomed();
        } else {
            int pw = x2 - x1 + 1;
            tft.startWrite();
            for (int row = y1; row <= y2; row++) {
                tft.setAddrWindow(CANVAS_X + x1, CANVAS_Y + row, pw, 1);
                tft.pushPixels(&canvas[row * CANVAS_W + x1], pw);
            }
            tft.endWrite();
        }
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
        if ((zoomLevel > 0)) {
            pushCanvasZoomed();
        } else {
            int pw = dX2 - dX1 + 1;
            tft.startWrite();
            for (int row = dY1; row <= dY2; row++) {
                tft.setAddrWindow(CANVAS_X + dX1, CANVAS_Y + row, pw, 1);
                tft.pushPixels(&canvas[row * CANVAS_W + dX1], pw);
            }
            tft.endWrite();
        }
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
    if ((zoomLevel > 0)) { pushCanvasZoomed(); return; }  // in zoom mode, just repaint whole viewport
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

// Ellipse outline — thickness = brushSize
// Uses scanline annulus: fills pixels between outer and inner ellipses — no gaps
void drawShapeEllipse(int cx, int cy, int rx, int ry, uint16_t color) {
    if (brushSize <= 1) {
        drawEllipseRing(cx, cy, rx, ry, color);
        return;
    }
    int t = (int)brushSize;
    int orx = rx, ory = ry;              // outer radii
    int irx = rx - t, iry = ry - t;      // inner radii
    // Scan bounding box of outer ellipse
    int yMin = max(0, cy - ory), yMax = min(CANVAS_H - 1, cy + ory);
    int xMin = max(0, cx - orx), xMax = min(CANVAS_W - 1, cx + orx);
    // Precompute squared radii as floats to avoid overflow
    float orx2 = (float)orx * orx, ory2 = (float)ory * ory;
    float irx2 = (irx > 0) ? (float)irx * irx : 0;
    float iry2 = (iry > 0) ? (float)iry * iry : 0;
    bool hasInner = (irx > 0 && iry > 0);
    for (int py = yMin; py <= yMax; py++) {
        float dy = (float)(py - cy);
        for (int px = xMin; px <= xMax; px++) {
            float dxf = (float)(px - cx);
            // Test: inside outer ellipse?
            float outerTest = (dxf * dxf) / orx2 + (dy * dy) / ory2;
            if (outerTest > 1.0f) continue;
            // Test: outside inner ellipse?
            if (hasInner) {
                float innerTest = (dxf * dxf) / irx2 + (dy * dy) / iry2;
                if (innerTest < 1.0f) continue;  // inside inner = hollow
            }
            canvasSet(px, py, color);
        }
    }
}
// Scanline flood fill — span queue algorithm.
// Processes whole horizontal spans instead of individual pixels, so the queue
// stays tiny (max ~800 spans for a 200x200 canvas) while covering all 40,000
// pixels reliably.  No heap allocation — uses a fixed static circular queue.
void floodFill(int x, int y, uint16_t newColor) {
    if (x < 0 || x >= CANVAS_W || y < 0 || y >= CANVAS_H) return;
    uint16_t oldColor = canvasGet(x, y);
    if (fillTolerance == 0 && oldColor == newColor) return;

    // v24: When using tolerance, need a visited bitmap
    uint8_t *visited = nullptr;
    if (fillTolerance > 0) {
        int maskBytes = (CANVAS_W * CANVAS_H + 7) / 8;
        visited = (uint8_t *)calloc(maskBytes, 1);
        // If alloc fails, fall back to exact mode
    }

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
    auto fmatch = [&](int px, int py) -> bool {
        if (visited) { int b=py*CANVAS_W+px; if((visited[b>>3]>>(b&7))&1) return false; }
        return colorMatch(canvasGet(px,py), oldColor);
    };
    auto fpaint = [&](int px, int py) {
        if (patternMode > 0) {
            if (patternCheck(px, py)) canvasSet(px, py, newColor);
            else canvasSet(px, py, secondColor);  // fill pattern gaps with secondary
        } else {
            canvasSet(px, py, newColor);
        }
        if (visited) { int b=py*CANVAS_W+px; visited[b>>3]|=(1<<(b&7)); }
    };
    while (lx > 0            && fmatch(lx-1, y)) lx--;
    while (rx < CANVAS_W-1   && fmatch(rx+1, y)) rx++;
    for (int i = lx; i <= rx; i++) fpaint(i, y);
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
            if (!fmatch(i, s.row)) { i++; continue; }
            int segL = i;
            while (i <= s.x2 && fmatch(i, s.row)) {
                fpaint(i, s.row);
                i++;
            }
            int segR = i - 1;
            int xl = segL, xr = segR;
            while (xl > 0          && fmatch(xl-1, s.row)) { xl--; fpaint(xl, s.row); }
            while (xr < CANVAS_W-1 && fmatch(xr+1, s.row)) { xr++; fpaint(xr, s.row); }
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
    if (visited) free(visited);
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

    // Canvas area — zoom pending: second tap to confirm zoom location
    if (rectHit(tx, ty, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H) && zoomPending) {
        int cx, cy;
        if ((zoomLevel > 0)) {
            cx = screenToCanvasX(tx);
            cy = screenToCanvasY(ty);
        } else {
            cx = tx - CANVAS_X;
            cy = ty - CANVAS_Y;
        }
        zoomIn(cx, cy);
        pushCanvas();
        drawBottomBar();
        return;
    }

    // Canvas area — only two-tap shape tools reach here via handleMainTouch.
    // Fill/Eyedropper are handled in loop() before debounce check.
    // Continuous tools (pen/eraser/spray) are also handled in loop().
    if (rectHit(tx, ty, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H)) {
        if (isTwoTapTool(activeTool)) {
            int cx, cy;
            if ((zoomLevel > 0)) {
                cx = screenToCanvasX(tx);
                cy = screenToCanvasY(ty);
            } else {
                cx = tx - CANVAS_X;
                cy = ty - CANVAS_Y;
            }
            cx = constrain(cx, 0, CANVAS_W - 1);
            cy = constrain(cy, 0, CANVAS_H - 1);

            if (!shapeHasOrigin) {
                shapeOriginX   = cx;
                shapeOriginY   = cy;
                shapeHasOrigin = true;
                // Draw crosshair at screen position (map canvas→screen if zoomed)
                int ox, oy;
                if ((zoomLevel > 0)) {
                    ox = CANVAS_X + (cx - zoomVX) * CANVAS_W / zoomVW;
                    oy = CANVAS_Y + (cy - zoomVY) * CANVAS_H / zoomVH;
                } else {
                    ox = CANVAS_X + cx;
                    oy = CANVAS_Y + cy;
                }
                tft.drawLine(ox - 5, oy, ox + 5, oy, C_ORANGE);
                tft.drawLine(ox, oy - 5, ox, oy + 5, C_ORANGE);
                drawBottomBar();
            } else {
                // Second tap: draw shape into canvas[], then push
                shapeHasOrigin = false; selCancel();
                undoPush();
                int ox = shapeOriginX, oy = shapeOriginY;
                // v24: Constrain mode
                if (constrainMode) {
                    int dx2 = abs(cx - ox), dy2 = abs(cy - oy);
                    if (activeTool == TOOL_RECT) {
                        // Square: use max dimension
                        int side = max(dx2, dy2);
                        cx = ox + (cx >= ox ? side : -side);
                        cy = oy + (cy >= oy ? side : -side);
                    } else if (activeTool == TOOL_ELLIPSE) {
                        // Circle: use average radius
                        int rad = (dx2 + dy2) / 2;
                        cx = ox + (cx >= ox ? rad : -rad);
                        cy = oy + (cy >= oy ? rad : -rad);
                    } else if (activeTool == TOOL_LINE) {
                        // Snap to nearest 45°
                        if (dx2 > dy2 * 2) cy = oy;           // horizontal
                        else if (dy2 > dx2 * 2) cx = ox;      // vertical
                        else { int d = (dx2+dy2)/2; cx = ox + (cx>=ox?d:-d); cy = oy + (cy>=oy?d:-d); }
                    }
                }
                int mx0 = CANVAS_W-1-ox, mx1 = CANVAS_W-1-cx;
                int my0 = CANVAS_H-1-oy, my1 = CANVAS_H-1-cy;

                auto drawShape = [&](int ax0, int ay0, int ax1, int ay1) {
                    if (activeTool == TOOL_LINE) {
                        drawShapeLine(ax0, ay0, ax1, ay1, activeColor);
                    } else if (activeTool == TOOL_RECT) {
                        // Fill with secondary color first, then outline
                        if (secondColor != C_WHITE) fillShapeRect(ax0, ay0, ax1, ay1);
                        drawShapeRect(ax0, ay0, ax1, ay1, activeColor);
                    } else if (activeTool == TOOL_ELLIPSE) {
                        int erx = abs(ax1 - ax0) / 2;
                        int ery = abs(ay1 - ay0) / 2;
                        int ecx = (ax0 + ax1) / 2;
                        int ecy = (ay0 + ay1) / 2;
                        if (secondColor != C_WHITE) fillShapeEllipse(ecx, ecy, erx, ery);
                        drawShapeEllipse(ecx, ecy, erx, ery, activeColor);
                    } else if (activeTool == TOOL_GRADIENT) {
                        if (constrainMode) {
                            // Gradient fill: flood-fill region under first tap, apply gradient
                            gradientFillRegion(ax0, ay0, ax0, ay0, ax1, ay1);
                        } else {
                            drawGradientLine(ax0, ay0, ax1, ay1);
                        }
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

    // Row 0: [<][PEN][>] [ToolName] [ColorSwatch] [BrushSize]  (unchanged)
    if (ry < 30) {
        if (rectHit(tx, ry, 0, 0, 30, 30)) {
            activeTool = (Tool)((activeTool + TOOL_COUNT - 1) % TOOL_COUNT);
            shapeHasOrigin = false; selCancel(); drawBottomBar(); return;
        }
        if (rectHit(tx, ry, 30, 0, 30, 30)) {
            activeTool = TOOL_PEN;
            shapeHasOrigin = false; selCancel(); drawBottomBar(); return;
        }
        if (rectHit(tx, ry, 60, 0, 30, 30)) {
            activeTool = (Tool)((activeTool + 1) % TOOL_COUNT);
            shapeHasOrigin = false; selCancel(); drawBottomBar(); return;
        }
        if (rectHit(tx, ry, 90, 0, 50, 30)) {
            drawToolSelectScreen(); return;
        }
        if (rectHit(tx, ry, 140, 0, 36, 15)) {
            cpSlot = 0;
            uint8_t r, g, b; rgb565to888(activeColor, r, g, b);
            cpR = r; cpG = g; cpB = b;
            drawColorPickScreen(); return;
        }
        if (rectHit(tx, ry, 140, 15, 36, 15)) {
            uint16_t tmp = activeColor;
            activeColor = secondColor;
            secondColor = tmp;
            drawBottomBar(); return;
        }
        if (rectHit(tx, ry, 176, 0, 18, 30)) {
            if (brushSize > 1) brushSize--;
            drawBottomBar(); return;
        }
        if (rectHit(tx, ry, 194, 0, 22, 30)) {
            drawBrushSelectScreen(); return;
        }
        if (rectHit(tx, ry, 216, 0, 24, 30)) {
            if (brushSize < 20) brushSize++;
            drawBottomBar(); return;
        }
        return;
    }

    // Row 1: [Undo(28)][Redo(28)] [Z+(28)][Z-(28)][GRD(28)] [FH/FV/R or LIN/RAD (54)] [Anim(46)]
    if (ry < 60) {
        int localY = ry - 30;
        if (rectHit(tx, localY, 0, 0, 28, 30)) { undoPerform(); drawBottomBar(); return; }
        if (rectHit(tx, localY, 28, 0, 28, 30)) { redoPerform(); drawBottomBar(); return; }
        if (rectHit(tx, localY, 56, 0, 28, 30)) {
            if (zoomLevel < ZOOM_MAX_LEVEL && !zoomPending) { zoomPending = true; drawBottomBar(); }
            else if (zoomPending) { zoomPending = false; drawBottomBar(); }
            return;
        }
        if (rectHit(tx, localY, 84, 0, 28, 30)) {
            if (zoomLevel > 0) { zoomOut(); pushCanvas(); drawBottomBar(); }
            else if (zoomPending) { zoomPending = false; drawBottomBar(); }
            return;
        }
        if (rectHit(tx, localY, 112, 0, 28, 30)) { gridOverlay = !gridOverlay; pushCanvas(); drawBottomBar(); return; }
        // Context area: sel transforms or gradient toggle
        bool selActive = (selBuf && (selPhase == SEL_PLACE || selPhase == SEL_LASSO_PLACE));
        if (selActive && rectHit(tx, localY, 140, 0, 18, 30)) {
            selFlipH(); pushCanvas(); selDrawOutline(); drawBottomBar(); return;
        }
        if (selActive && rectHit(tx, localY, 158, 0, 18, 30)) {
            selFlipV(); pushCanvas(); selDrawOutline(); drawBottomBar(); return;
        }
        if (selActive && rectHit(tx, localY, 176, 0, 18, 30)) {
            selRotate90(); pushCanvas(); selDrawOutline(); drawBottomBar(); return;
        }
        if (!selActive && rectHit(tx, localY, 140, 0, 54, 30)) {
            gradientRadial = !gradientRadial; drawBottomBar(); return;
        }
        if (rectHit(tx, localY, 194, 0, 46, 30)) { animCommitFrame(); drawAnimScreen(); return; }
        return;
    }

    // Row 2: [SV(24)][LD(24)][NEW(26)][CLR(26)] [Stab(24)][Pat(28)][X/C(16)][Con(18)][Tol(28)][OS(26)]
    if (ry < 90) {
        int localY = ry - 60;
        if (rectHit(tx, localY, 0, 0, 24, 30)) { saveFile(); drawBottomBar(); return; }
        if (rectHit(tx, localY, 24, 0, 24, 30)) {
            galleryMode = 0; galleryPage = 0; scanFiles();
            drawGalleryScreen(); return;
        }
        if (rectHit(tx, localY, 48, 0, 26, 30)) {
            confirmAction = CA_NEW; drawConfirmScreen("New canvas? Unsaved work lost."); return;
        }
        if (rectHit(tx, localY, 74, 0, 26, 30)) {
            confirmAction = CA_CLR; drawConfirmScreen("Clear to white?"); return;
        }
        if (rectHit(tx, localY, 100, 0, 24, 30)) { stabilizerLevel = (stabilizerLevel + 1) % 3; drawBottomBar(); return; }
        if (rectHit(tx, localY, 124, 0, 28, 30)) { patternMode = (patternMode + 1) % PATTERN_COUNT; drawBottomBar(); return; }
        if (rectHit(tx, localY, 152, 0, 16, 30)) { selCutMode = !selCutMode; drawBottomBar(); return; }
        if (rectHit(tx, localY, 168, 0, 18, 30)) { constrainMode = !constrainMode; drawBottomBar(); return; }
        if (rectHit(tx, localY, 186, 0, 28, 30)) {
            fillTolerance = (fillTolerance + 4) % 28; drawBottomBar(); return;
        }
        if (rectHit(tx, localY, 214, 0, 26, 30)) {
            onionSkin = !onionSkin; pushCanvas(); drawBottomBar(); return;
        }
        return;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// v24: Gradient, Polygon, Curve, Shape Fill
// ─────────────────────────────────────────────────────────────────────────────

// Gradient tool — fills canvas rect between two points with color interpolation
void drawGradientLine(int x0, int y0, int x1, int y1) {
    // Extract RGB565 components
    int r0 = (activeColor >> 11) & 0x1F, g0 = (activeColor >> 5) & 0x3F, b0 = activeColor & 0x1F;
    int r1 = (secondColor >> 11) & 0x1F, g1 = (secondColor >> 5) & 0x3F, b1 = secondColor & 0x1F;

    if (gradientRadial) {
        // Radial gradient: point 1 = center, point 2 = edge
        int cxR = x0, cyR = y0;
        float maxDist = sqrtf((float)(x1-x0)*(x1-x0) + (float)(y1-y0)*(y1-y0));
        if (maxDist < 1.0f) maxDist = 1.0f;
        for (int py = 0; py < CANVAS_H; py++) {
            for (int px = 0; px < CANVAS_W; px++) {
                float dist = sqrtf((float)(px-cxR)*(px-cxR) + (float)(py-cyR)*(py-cyR));
                float t = dist / maxDist;
                if (t > 1.0f) t = 1.0f;
                int rr = r0 + (int)((r1-r0)*t);
                int gg = g0 + (int)((g1-g0)*t);
                int bb = b0 + (int)((b1-b0)*t);
                rr = constrain(rr, 0, 31); gg = constrain(gg, 0, 63); bb = constrain(bb, 0, 31);
                canvasSet(px, py, (rr<<11)|(gg<<5)|bb);
            }
        }
    } else {
        // Linear gradient: fills entire canvas along the direction from p1 to p2
        // Project each pixel onto the line p1→p2
        float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
        float lenSq = dx*dx + dy*dy;
        if (lenSq < 1.0f) lenSq = 1.0f;
        for (int py = 0; py < CANVAS_H; py++) {
            for (int px = 0; px < CANVAS_W; px++) {
                // Project (px,py) onto the gradient line
                float proj = ((px-x0)*dx + (py-y0)*dy) / lenSq;
                if (proj < 0.0f) proj = 0.0f;
                if (proj > 1.0f) proj = 1.0f;
                int rr = r0 + (int)((r1-r0)*proj);
                int gg = g0 + (int)((g1-g0)*proj);
                int bb = b0 + (int)((b1-b0)*proj);
                rr = constrain(rr, 0, 31); gg = constrain(gg, 0, 63); bb = constrain(bb, 0, 31);
                canvasSet(px, py, (rr<<11)|(gg<<5)|bb);
            }
        }
    }
}

// Gradient fill — flood-fill a region, then apply gradient to only those pixels
void gradientFillRegion(int seedX, int seedY, int x0, int y0, int x1, int y1) {
    if (seedX < 0 || seedX >= CANVAS_W || seedY < 0 || seedY >= CANVAS_H) return;
    uint16_t target = canvasGet(seedX, seedY);

    // Build visited mask via flood fill
    int maskBytes = (CANVAS_W * CANVAS_H + 7) / 8;
    uint8_t *vis = (uint8_t *)calloc(maskBytes, 1);
    if (!vis) { setStatus("No mem"); return; }

    struct Span { int16_t x1, x2, row, dy; };
    static Span fq[800];
    int head = 0, tail = 0;
    #define GFQ_PUSH(a,b,r,d) { fq[tail]={int16_t(a),int16_t(b),int16_t(r),int16_t(d)}; tail=(tail+1)%800; }
    #define GFQ_POP(s) { s=fq[head]; head=(head+1)%800; }
    #define GFQ_EMPTY (head==tail)
    #define GFV_SET(px,py) { int _b=(py)*CANVAS_W+(px); vis[_b>>3]|=(1<<(_b&7)); }
    #define GFV_GET(px,py) ((vis[((py)*CANVAS_W+(px))>>3]>>(((py)*CANVAS_W+(px))&7))&1)

    auto gfMatch = [&](int px, int py) -> bool {
        if (GFV_GET(px,py)) return false;
        return colorMatch(canvasGet(px,py), target);
    };

    int lx=seedX, rx=seedX;
    while(lx>0 && gfMatch(lx-1,seedY)) lx--;
    while(rx<CANVAS_W-1 && gfMatch(rx+1,seedY)) rx++;
    for(int i=lx;i<=rx;i++) GFV_SET(i,seedY);
    if(seedY>0) GFQ_PUSH(lx,rx,seedY-1,-1)
    if(seedY<CANVAS_H-1) GFQ_PUSH(lx,rx,seedY+1,+1)

    while(!GFQ_EMPTY) {
        Span s; GFQ_POP(s)
        int i=s.x1;
        while(i<=s.x2) {
            if(!gfMatch(i,s.row)){i++;continue;}
            int sL=i;
            while(i<=s.x2 && gfMatch(i,s.row)){GFV_SET(i,s.row);i++;}
            int sR=i-1, xl=sL, xr=sR;
            while(xl>0 && gfMatch(xl-1,s.row)){xl--;GFV_SET(xl,s.row);}
            while(xr<CANVAS_W-1 && gfMatch(xr+1,s.row)){xr++;GFV_SET(xr,s.row);}
            int nr=s.row+s.dy; if(nr>=0&&nr<CANVAS_H) GFQ_PUSH(xl,xr,nr,s.dy)
            int pr=s.row-s.dy; if(pr>=0&&pr<CANVAS_H&&(xl<s.x1||xr>s.x2)) GFQ_PUSH(xl,xr,pr,-s.dy)
        }
    }
    #undef GFQ_PUSH
    #undef GFQ_POP
    #undef GFQ_EMPTY

    // Now apply gradient only to visited pixels
    int r0c = (activeColor >> 11) & 0x1F, g0c = (activeColor >> 5) & 0x3F, b0c = activeColor & 0x1F;
    int r1c = (secondColor >> 11) & 0x1F, g1c = (secondColor >> 5) & 0x3F, b1c = secondColor & 0x1F;

    if (gradientRadial) {
        float maxDist = sqrtf((float)(x1-x0)*(x1-x0) + (float)(y1-y0)*(y1-y0));
        if (maxDist < 1.0f) maxDist = 1.0f;
        for (int py = 0; py < CANVAS_H; py++) {
            for (int px = 0; px < CANVAS_W; px++) {
                if (!GFV_GET(px,py)) continue;
                float dist = sqrtf((float)(px-x0)*(px-x0) + (float)(py-y0)*(py-y0));
                float t = dist / maxDist;
                if (t > 1.0f) t = 1.0f;
                int rr = r0c+(int)((r1c-r0c)*t), gg = g0c+(int)((g1c-g0c)*t), bb = b0c+(int)((b1c-b0c)*t);
                canvasSet(px, py, (constrain(rr,0,31)<<11)|(constrain(gg,0,63)<<5)|constrain(bb,0,31));
            }
        }
    } else {
        float dxf = (float)(x1-x0), dyf = (float)(y1-y0);
        float lenSq = dxf*dxf + dyf*dyf;
        if (lenSq < 1.0f) lenSq = 1.0f;
        for (int py = 0; py < CANVAS_H; py++) {
            for (int px = 0; px < CANVAS_W; px++) {
                if (!GFV_GET(px,py)) continue;
                float proj = ((px-x0)*dxf + (py-y0)*dyf) / lenSq;
                if (proj < 0.0f) proj = 0.0f;
                if (proj > 1.0f) proj = 1.0f;
                int rr = r0c+(int)((r1c-r0c)*proj), gg = g0c+(int)((g1c-g0c)*proj), bb = b0c+(int)((b1c-b0c)*proj);
                canvasSet(px, py, (constrain(rr,0,31)<<11)|(constrain(gg,0,63)<<5)|constrain(bb,0,31));
            }
        }
    }
    free(vis);
    #undef GFV_SET
    #undef GFV_GET
}

// Polygon tool — draw edges between all vertices
void drawPolygon() {
    if (polyPtCount < 2) return;
    for (int i = 0; i < polyPtCount; i++) {
        int j = (i + 1) % polyPtCount;
        drawShapeLine(polyX[i], polyY[i], polyX[j], polyY[j], activeColor);
    }
}

// Bezier curve (quadratic: start, end, control point)
void drawBezierCurve(int x0, int y0, int x1, int y1, int cx, int cy) {
    int prevX = x0, prevY = y0;
    int steps = 40;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps;
        float u = 1.0f - t;
        int bx = (int)(u*u*x0 + 2*u*t*cx + t*t*x1 + 0.5f);
        int by = (int)(u*u*y0 + 2*u*t*cy + t*t*y1 + 0.5f);
        drawShapeLine(prevX, prevY, bx, by, activeColor);
        prevX = bx; prevY = by;
    }
}

// Fill rectangle interior with secondary color
void fillShapeRect(int x0, int y0, int x1, int y1) {
    if (x0 > x1) { int t=x0; x0=x1; x1=t; }
    if (y0 > y1) { int t=y0; y0=y1; y1=t; }
    int t = max(1, (int)brushSize - 1);
    int fx0 = x0+t, fy0 = y0+t, fx1 = x1-t, fy1 = y1-t;
    for (int y = fy0; y <= fy1; y++) {
        for (int x = fx0; x <= fx1; x++) {
            if (x >= 0 && x < CANVAS_W && y >= 0 && y < CANVAS_H)
                canvasSet(x, y, secondColor);
        }
    }
}

// Fill ellipse interior with secondary color (inside the stroke border)
void fillShapeEllipse(int cx, int cy, int rx, int ry) {
    int t = (int)brushSize;
    int irx = rx - t, iry = ry - t;
    if (irx <= 0 || iry <= 0) return;
    float irx2 = (float)irx * irx, iry2 = (float)iry * iry;
    int yMin = max(0, cy - iry), yMax = min(CANVAS_H - 1, cy + iry);
    int xMin = max(0, cx - irx), xMax = min(CANVAS_W - 1, cx + irx);
    for (int py = yMin; py <= yMax; py++) {
        float dy = (float)(py - cy);
        for (int px = xMin; px <= xMax; px++) {
            float dxf = (float)(px - cx);
            if ((dxf * dxf) / irx2 + (dy * dy) / iry2 <= 1.0f)
                canvasSet(px, py, secondColor);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Tool Select Screen — grid of all tools, tap to select
// ─────────────────────────────────────────────────────────────────────────────
void drawToolSelectScreen() {
    currentScreen = SCR_TOOL_SEL;
    tft.fillScreen(0x1082);
    tft.setTextColor(C_WHITE, 0x1082); tft.setTextSize(1);
    tft.setCursor(4, 4); tft.print("SELECT TOOL");

    int cols = 3, bw = 78, bh = 32, gap = 2, startY = 20;
    for (int i = 0; i < TOOL_COUNT; i++) {
        int col = i % cols;
        int row = i / cols;
        int bx = col * (bw + gap) + 2;
        int by = startY + row * (bh + gap);
        bool active = (toolDisplayOrder[i] == activeTool);
        uint16_t bg = active ? toolDisplayColors[i] : 0x4208;
        uint16_t fg = active ? C_BLACK : toolDisplayColors[i];
        drawButtonRect(bx, by, bw, bh, bg, fg, toolDisplayNames[i]);
    }

    drawButtonRect(60, 280, 120, 30, 0x4208, C_WHITE, "CANCEL");
}

void handleToolSelectTouch(int tx, int ty) {
    int cols = 3, bw = 78, bh = 32, gap = 2, startY = 20;
    for (int i = 0; i < TOOL_COUNT; i++) {
        int col = i % cols;
        int row = i / cols;
        int bx = col * (bw + gap) + 2;
        int by = startY + row * (bh + gap);
        if (rectHit(tx, ty, bx, by, bw, bh)) {
            activeTool = toolDisplayOrder[i];
            shapeHasOrigin = false; selCancel(); polyPtCount = 0; curvePhase = 0;
            drawMainScreen();
            return;
        }
    }
    if (rectHit(tx, ty, 60, 280, 120, 30)) {
        drawMainScreen();
        return;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// Text Input Screen — on-screen keyboard + preview
// ─────────────────────────────────────────────────────────────────────────────

// Render text into canvas at (x,y). Supports newlines (\n).
// Font size linked to brushSize: 1-3=small(1), 4-7=med(2), 8+=large(3).
// textFont: 0=normal, 1=bold(double-strike), 2=wide.
void stampText(int tx2, int ty2) {
    if (textLen <= 0) return;
    int sz;
    if (brushSize <= 3) sz = 1;
    else if (brushSize <= 7) sz = 2;
    else sz = 3;
    int renderSz = sz;
    bool bold = (textFont == 1);
    if (textFont == 2) renderSz = sz + 1;

    // Embedded 5x7 font (ASCII 32-126), 5 bytes per char, each byte = column, LSB = top row
    static const uint8_t font5x7[] PROGMEM = {
        0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x5F,0x00,0x00, // sp !
        0x00,0x07,0x00,0x07,0x00, 0x14,0x7F,0x14,0x7F,0x14, // " #
        0x24,0x2A,0x7F,0x2A,0x12, 0x23,0x13,0x08,0x64,0x62, // $ %
        0x36,0x49,0x55,0x22,0x50, 0x00,0x05,0x03,0x00,0x00, // & '
        0x00,0x1C,0x22,0x41,0x00, 0x00,0x41,0x22,0x1C,0x00, // ( )
        0x08,0x2A,0x1C,0x2A,0x08, 0x08,0x08,0x3E,0x08,0x08, // * +
        0x00,0x50,0x30,0x00,0x00, 0x08,0x08,0x08,0x08,0x08, // , -
        0x00,0x60,0x60,0x00,0x00, 0x20,0x10,0x08,0x04,0x02, // . /
        0x3E,0x51,0x49,0x45,0x3E, 0x00,0x42,0x7F,0x40,0x00, // 0 1
        0x42,0x61,0x51,0x49,0x46, 0x21,0x41,0x45,0x4B,0x31, // 2 3
        0x18,0x14,0x12,0x7F,0x10, 0x27,0x45,0x45,0x45,0x39, // 4 5
        0x3C,0x4A,0x49,0x49,0x30, 0x01,0x71,0x09,0x05,0x03, // 6 7
        0x36,0x49,0x49,0x49,0x36, 0x06,0x49,0x49,0x29,0x1E, // 8 9
        0x00,0x36,0x36,0x00,0x00, 0x00,0x56,0x36,0x00,0x00, // : ;
        0x00,0x08,0x14,0x22,0x41, 0x14,0x14,0x14,0x14,0x14, // < =
        0x41,0x22,0x14,0x08,0x00, 0x02,0x01,0x51,0x09,0x06, // > ?
        0x32,0x49,0x79,0x41,0x3E, // @
        0x7E,0x11,0x11,0x11,0x7E, 0x7F,0x49,0x49,0x49,0x36, // A B
        0x3E,0x41,0x41,0x41,0x22, 0x7F,0x41,0x41,0x22,0x1C, // C D
        0x7F,0x49,0x49,0x49,0x41, 0x7F,0x09,0x09,0x01,0x01, // E F
        0x3E,0x41,0x41,0x51,0x32, 0x7F,0x08,0x08,0x08,0x7F, // G H
        0x00,0x41,0x7F,0x41,0x00, 0x20,0x40,0x41,0x3F,0x01, // I J
        0x7F,0x08,0x14,0x22,0x41, 0x7F,0x40,0x40,0x40,0x40, // K L
        0x7F,0x02,0x04,0x02,0x7F, 0x7F,0x04,0x08,0x10,0x7F, // M N
        0x3E,0x41,0x41,0x41,0x3E, 0x7F,0x09,0x09,0x09,0x06, // O P
        0x3E,0x41,0x51,0x21,0x5E, 0x7F,0x09,0x19,0x29,0x46, // Q R
        0x46,0x49,0x49,0x49,0x31, 0x01,0x01,0x7F,0x01,0x01, // S T
        0x3F,0x40,0x40,0x40,0x3F, 0x1F,0x20,0x40,0x20,0x1F, // U V
        0x7F,0x20,0x18,0x20,0x7F, 0x63,0x14,0x08,0x14,0x63, // W X
        0x03,0x04,0x78,0x04,0x03, 0x61,0x51,0x49,0x45,0x43, // Y Z
        0x00,0x00,0x7F,0x41,0x41, 0x02,0x04,0x08,0x10,0x20, // [ bkslash
        0x41,0x41,0x7F,0x00,0x00, 0x04,0x02,0x01,0x02,0x04, // ] ^
        0x40,0x40,0x40,0x40,0x40, 0x00,0x01,0x02,0x04,0x00, // _ `
        0x20,0x54,0x54,0x54,0x78, 0x7F,0x48,0x44,0x44,0x38, // a b
        0x38,0x44,0x44,0x44,0x20, 0x38,0x44,0x44,0x48,0x7F, // c d
        0x38,0x54,0x54,0x54,0x18, 0x08,0x7E,0x09,0x01,0x02, // e f
        0x08,0x14,0x54,0x54,0x3C, 0x7F,0x08,0x04,0x04,0x78, // g h
        0x00,0x44,0x7D,0x40,0x00, 0x20,0x40,0x44,0x3D,0x00, // i j
        0x00,0x7F,0x10,0x28,0x44, 0x00,0x41,0x7F,0x40,0x00, // k l
        0x7C,0x04,0x18,0x04,0x78, 0x7C,0x08,0x04,0x04,0x78, // m n
        0x38,0x44,0x44,0x44,0x38, 0x7C,0x14,0x14,0x14,0x08, // o p
        0x08,0x14,0x14,0x18,0x7C, 0x7C,0x08,0x04,0x04,0x08, // q r
        0x48,0x54,0x54,0x54,0x20, 0x04,0x3F,0x44,0x40,0x20, // s t
        0x3C,0x40,0x40,0x20,0x7C, 0x1C,0x20,0x40,0x20,0x1C, // u v
        0x3C,0x40,0x30,0x40,0x3C, 0x44,0x28,0x10,0x28,0x44, // w x
        0x0C,0x50,0x50,0x50,0x3C, 0x44,0x64,0x54,0x4C,0x44, // y z
        0x00,0x08,0x36,0x41,0x00, 0x00,0x00,0x7F,0x00,0x00, // { |
        0x00,0x41,0x36,0x08,0x00, 0x08,0x08,0x2A,0x1C,0x08, // } ~
    };

    int charW = 6 * renderSz;
    int charH = 8 * renderSz;
    int lineH = charH + renderSz;  // line spacing
    int curX = tx2, curY = ty2;

    for (int ci = 0; ci < textLen; ci++) {
        char ch = textBuf[ci];
        if (ch == '\n') { curX = tx2; curY += lineH; continue; }
        if (ch < 32 || ch > 126) ch = '?';
        for (int col = 0; col < 5; col++) {
            uint8_t colBits = pgm_read_byte(&font5x7[(ch - 32) * 5 + col]);
            for (int row = 0; row < 7; row++) {
                if (colBits & (1 << row)) {
                    for (int sy = 0; sy < renderSz; sy++) {
                        for (int sx = 0; sx < renderSz; sx++) {
                            int px = curX + col * renderSz + sx;
                            int py = curY + row * renderSz + sy;
                            if (px >= 0 && px < CANVAS_W && py >= 0 && py < CANVAS_H)
                                canvasSet(px, py, activeColor);
                            if (bold && px+1 >= 0 && px+1 < CANVAS_W && py >= 0 && py < CANVAS_H)
                                canvasSet(px + 1, py, activeColor);
                        }
                    }
                }
            }
        }
        curX += charW;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Text Input Screen — on-screen keyboard with multiline support
// ─────────────────────────────────────────────────────────────────────────────

// Helper: count lines and find line starts
static int textLineCount() {
    int n = 1;
    for (int i = 0; i < textLen; i++) if (textBuf[i] == '\n') n++;
    return n;
}

void drawTextInputScreen() {
    currentScreen = SCR_TEXT_INPUT;
    tft.fillScreen(0x1082);
    tft.setTextColor(C_WHITE, 0x1082); tft.setTextSize(1);
    tft.setCursor(4, 2); tft.print("TEXT TOOL");

    // Preview area — multiline, up to 8 visible lines
    int prevH = 82;
    tft.fillRect(4, 14, 232, prevH, 0x2104);
    tft.drawRect(4, 14, 232, prevH, C_DKGRAY);
    tft.setTextColor(activeColor, 0x2104);
    tft.setTextSize(1);
    int px = 8, py = 17, charIdx = 0;
    for (int i = 0; i < textLen; i++) {
        if (i == textCursor) {
            tft.drawFastVLine(px, py - 1, 10, C_CYAN);  // cursor
        }
        if (textBuf[i] == '\n') {
            px = 8; py += 10;
        } else {
            if (px < 230) { tft.setCursor(px, py); tft.print(textBuf[i]); }
            px += 6;
        }
    }
    if (textCursor == textLen) {
        tft.drawFastVLine(px, py - 1, 10, C_CYAN);
    }

    // Font selector row
    int fontY = 14 + prevH + 2;
    const char *fontLbl[] = {"Normal", "Bold", "Wide"};
    for (int fi = 0; fi < TEXT_FONT_COUNT; fi++) {
        uint16_t bg = (textFont == fi) ? C_CYAN : 0x4208;
        uint16_t fg = (textFont == fi) ? C_BLACK : C_WHITE;
        drawButtonRect(4 + fi * 78, fontY, 76, 16, bg, fg, fontLbl[fi]);
    }

    // Keyboard — 7 rows of character keys (max 10 keys/row so no clipping)
    //   Row 0: 1 2 3 4 5 6 7 8 9 0
    //   Row 1: Q W E R T Y U I O P
    //   Row 2: A S D F G H J K L
    //   Row 3: Z X C V B N M
    //   Row 4: . , ? ! : ; ' - _ "
    //   Row 5: / @ # $ % & * ( ) =
    //   Row 6: + = < > [ ] { } \ `
    static const char *kbRows[] = {
        "1234567890",
        "QWERTYUIOP",
        "ASDFGHJKL",
        "ZXCVBNM",
        ".,?!:;'-_\"",
        "/@#$%&*()=",
        "+=<>[]{}\\`"
    };
    int kbRowCount = (int)(sizeof(kbRows) / sizeof(kbRows[0]));
    int kbY = fontY + 19;
    int keyW = 22, keyH = 18, kgap = 1;
    for (int r = 0; r < kbRowCount; r++) {
        const char *row = kbRows[r];
        int rlen = strlen(row);
        int rowX = (240 - rlen * (keyW + kgap)) / 2;
        for (int k = 0; k < rlen; k++) {
            char ch = row[k];
            char lbl[2] = {ch, 0};
            // Show lowercase on letter keys when lowercase mode
            if (textLowerCase && ch >= 'A' && ch <= 'Z') lbl[0] = ch + 32;
            drawButtonRect(rowX + k * (keyW + kgap), kbY + r * (keyH + kgap),
                           keyW, keyH, 0x4208, C_WHITE, lbl);
        }
    }

    // Action buttons row
    int btnY = kbY + kbRowCount * (keyH + kgap) + 2;
    drawButtonRect(4,   btnY, 38, 24, 0x4208, C_WHITE,  "SPC");
    drawButtonRect(44,  btnY, 28, 24, 0x4208, C_LIME,   "ENT");
    drawButtonRect(74,  btnY, 38, 24, 0x4208, C_ORANGE, "BKSP");
    drawButtonRect(114, btnY, 34, 24, 0x4208, C_RED,    "CLR");
    // Shift indicator: highlight when uppercase (default), dim when lowercase
    uint16_t shBg = textLowerCase ? 0x4208 : C_YELLOW;
    uint16_t shFg = textLowerCase ? C_DKGRAY : C_BLACK;
    const char *shLbl = textLowerCase ? "abc" : "ABC";
    drawButtonRect(150, btnY, 36, 24, shBg, shFg, shLbl);
    drawButtonRect(188, btnY, 48, 24, C_GREEN, C_BLACK,  "OK");
}

void handleTextInputTouch(int tx, int ty) {
    int prevH = 82;
    int fontY = 14 + prevH + 2;

    // Font selector
    for (int fi = 0; fi < TEXT_FONT_COUNT; fi++) {
        if (rectHit(tx, ty, 4 + fi * 78, fontY, 76, 16)) {
            textFont = fi;
            drawTextInputScreen(); return;
        }
    }

    // Keyboard
    static const char *kbRows[] = {
        "1234567890",
        "QWERTYUIOP",
        "ASDFGHJKL",
        "ZXCVBNM",
        ".,?!:;'-_\"",
        "/@#$%&*()=",
        "+=<>[]{}\\`"
    };
    int kbRowCount = (int)(sizeof(kbRows) / sizeof(kbRows[0]));
    int kbY = fontY + 19;
    int keyW = 22, keyH = 18, kgap = 1;
    for (int r = 0; r < kbRowCount; r++) {
        const char *row = kbRows[r];
        int rlen = strlen(row);
        int rowX = (240 - rlen * (keyW + kgap)) / 2;
        for (int k = 0; k < rlen; k++) {
            int bx = rowX + k * (keyW + kgap);
            int byK = kbY + r * (keyH + kgap);
            if (rectHit(tx, ty, bx, byK, keyW, keyH)) {
                if (textLen < TEXT_MAX_LEN) {
                    char ch = row[k];
                    if (textLowerCase && ch >= 'A' && ch <= 'Z') ch += 32;
                    for (int j = textLen; j > textCursor; j--) textBuf[j] = textBuf[j-1];
                    textBuf[textCursor] = ch;
                    textLen++; textCursor++;
                    textBuf[textLen] = 0;
                }
                drawTextInputScreen(); return;
            }
        }
    }

    // Action buttons
    int btnY = kbY + kbRowCount * (keyH + kgap) + 2;

    // SPC
    if (rectHit(tx, ty, 4, btnY, 38, 24)) {
        if (textLen < TEXT_MAX_LEN) {
            for (int j = textLen; j > textCursor; j--) textBuf[j] = textBuf[j-1];
            textBuf[textCursor] = ' ';
            textLen++; textCursor++;
            textBuf[textLen] = 0;
        }
        drawTextInputScreen(); return;
    }
    // ENT (newline)
    if (rectHit(tx, ty, 44, btnY, 28, 24)) {
        if (textLen < TEXT_MAX_LEN && textLineCount() < 8) {
            for (int j = textLen; j > textCursor; j--) textBuf[j] = textBuf[j-1];
            textBuf[textCursor] = '\n';
            textLen++; textCursor++;
            textBuf[textLen] = 0;
        }
        drawTextInputScreen(); return;
    }
    // BKSP
    if (rectHit(tx, ty, 74, btnY, 38, 24)) {
        if (textCursor > 0 && textLen > 0) {
            for (int j = textCursor - 1; j < textLen - 1; j++) textBuf[j] = textBuf[j+1];
            textLen--; textCursor--;
            textBuf[textLen] = 0;
        }
        drawTextInputScreen(); return;
    }
    // CLR
    if (rectHit(tx, ty, 114, btnY, 34, 24)) {
        textLen = 0; textCursor = 0; textBuf[0] = 0;
        drawTextInputScreen(); return;
    }
    // Shift toggle (ABC/abc)
    if (rectHit(tx, ty, 150, btnY, 36, 24)) {
        textLowerCase = !textLowerCase;
        drawTextInputScreen(); return;
    }
    // OK
    if (rectHit(tx, ty, 188, btnY, 48, 24)) {
        if (textLen > 0) {
            undoPush();
            stampText(shapeOriginX, shapeOriginY);
            pushCanvas();
        }
        drawMainScreen(); return;
    }

    // Tap on preview area moves cursor
    if (rectHit(tx, ty, 4, 14, 232, prevH)) {
        // Approximate cursor from tap position
        int tapLine = (ty - 17) / 10;
        int tapCol = (tx - 8) / 6;
        if (tapCol < 0) tapCol = 0;
        // Walk through buffer to find matching position
        int line = 0, col = 0;
        int bestI = 0;
        for (int i = 0; i <= textLen; i++) {
            if (line == tapLine && col >= tapCol) { bestI = i; break; }
            if (i == textLen) { bestI = textLen; break; }
            if (textBuf[i] == '\n') {
                if (line == tapLine) { bestI = i; break; }
                line++; col = 0;
            } else {
                col++;
            }
            bestI = i + 1;
        }
        if (bestI > textLen) bestI = textLen;
        textCursor = bestI;
        drawTextInputScreen(); return;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// Brush Size Select Screen — grid of sizes + preview
// ─────────────────────────────────────────────────────────────────────────────
void drawBrushSelectScreen() {
    currentScreen = SCR_BRUSH_SEL;
    tft.fillScreen(0x1082);
    tft.setTextColor(C_WHITE, 0x1082); tft.setTextSize(1);
    tft.setCursor(4, 4); tft.print("BRUSH SIZE");

    // Size buttons: 1-20 in a grid
    int cols = 5, bw = 46, bh = 30, gap = 2, startY = 24;
    for (int sz = 1; sz <= 20; sz++) {
        int idx = sz - 1;
        int col = idx % cols;
        int row = idx / cols;
        int bx = col * (bw + gap) + 2;
        int by = startY + row * (bh + gap);
        bool active = (brushSize == sz);
        char lbl[4]; snprintf(lbl, 4, "%d", sz);
        drawButtonRect(bx, by, bw, bh, active ? C_CYAN : 0x4208,
                       active ? C_BLACK : C_WHITE, lbl);
    }

    // Preview area
    int prevY = startY + 4 * (bh + gap) + 8;
    tft.fillRect(10, prevY, 220, 60, 0x2104);
    tft.drawRect(10, prevY, 220, 60, C_DKGRAY);
    int prevR = min((int)brushSize - 1, 28);
    if (prevR < 0) prevR = 0;
    tft.fillCircle(120, prevY + 30, prevR, activeColor);
    tft.drawCircle(120, prevY + 30, prevR, C_WHITE);
    tft.setTextColor(C_WHITE, 0x2104);
    tft.setCursor(14, prevY + 4);
    tft.printf("Size: %d px", brushSize);

    // OK button
    drawButtonRect(60, 280, 120, 30, C_GREEN, C_BLACK, "OK");
}

void handleBrushSelectTouch(int tx, int ty) {
    int cols = 5, bw = 46, bh = 30, gap = 2, startY = 24;
    for (int sz = 1; sz <= 20; sz++) {
        int idx = sz - 1;
        int col = idx % cols;
        int row = idx / cols;
        int bx = col * (bw + gap) + 2;
        int by = startY + row * (bh + gap);
        if (rectHit(tx, ty, bx, by, bw, bh)) {
            brushSize = sz;
            drawBrushSelectScreen();
            return;
        }
    }
    // OK
    if (rectHit(tx, ty, 60, 280, 120, 30)) {
        drawMainScreen();
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Colour Picker Screen
// ─────────────────────────────────────────────────────────────────────────────
void drawColorPickScreen() {
    currentScreen = SCR_COLOR_PICK;
    zoomPending = false;  // cancel any pending zoom
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

// scanFiles — populate galleryFiles with PAINT*.BMP filenames
void scanFiles() {
    galleryCount = 0;
    if (!sdOK) return;
    File root = SD.open("/");
    while (galleryCount < 128) {
        File f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            const char *nm = f.name();
            int len = strlen(nm);
            if (len > 4 && strcasecmp(nm + len - 4, ".bmp") == 0) {
                // Show all BMPs EXCEPT animation frames (ANMnnn_ff.BMP)
                bool isAnimFrame = (len == 13 && strncasecmp(nm, "ANM", 3) == 0 && nm[6] == '_');
                if (!isAnimFrame) {
                    strncpy(galleryFiles[galleryCount], nm, 15);
                    galleryFiles[galleryCount][15] = 0;
                    galleryCount++;
                }
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
            if (len > 4 && strcasecmp(nm + len - 4, ".cyd") == 0) cydCount++;
        }
        f.close();
    }
    root.close();
}

#define GALLERY_PER_PAGE 10   // text list — fits 10 items per page (room for extra nav row)

void galleryClearSel() {
    for (int i = 0; i < 128; i++) gallerySel[i] = false;
    gallerySelMode = false;
}

void drawGalleryScreen() {
    currentScreen = SCR_GALLERY;
    tft.fillScreen(0x1082);
    tft.setTextColor(C_WHITE, 0x1082);
    tft.setTextSize(1);
    tft.setCursor(4, 2);
    int totalPages = max(1, (galleryCount + GALLERY_PER_PAGE - 1) / GALLERY_PER_PAGE);
    const char *modeLabels[] = {"IMG", "CYD"};
    const char *modeLabel = modeLabels[galleryMode];
    tft.printf("%s pg%d/%d (%d)", modeLabel, galleryPage + 1, totalPages, galleryCount);

    // Text list — each row is 22px tall
    int startIdx = galleryPage * GALLERY_PER_PAGE;
    int listY = 14;
    for (int i = 0; i < GALLERY_PER_PAGE && startIdx + i < galleryCount; i++) {
        int ey = listY + i * 22;
        int idx = startIdx + i;
        uint16_t rowBg = (i & 1) ? 0x18E3 : 0x1082;
        if (gallerySelMode && gallerySel[idx]) rowBg = 0x3186;  // highlight selected
        tft.fillRect(0, ey, SCREEN_W, 22, rowBg);
        // Selection checkbox or file name
        if (gallerySelMode) {
            tft.drawRect(2, ey + 5, 12, 12, C_WHITE);
            if (gallerySel[idx]) {
                tft.drawLine(4, ey+11, 8, ey+15, C_GREEN);
                tft.drawLine(8, ey+15, 12, ey+7, C_GREEN);
            }
            tft.setTextColor(galleryMode > 0 ? C_CYAN : C_WHITE, rowBg);
            tft.setCursor(16, ey + 7);
        } else {
            tft.setTextColor(galleryMode > 0 ? C_CYAN : C_WHITE, rowBg);
            tft.setCursor(2, ey + 7);
        }
        tft.print(galleryFiles[idx]);
        // Buttons on right: LD / RN / DEL (or just checkbox area in sel mode)
        if (!gallerySelMode) {
            drawButtonRect(144, ey + 1, 32, 20, 0x4208, C_GREEN,  "LD");
            drawButtonRect(176, ey + 1, 32, 20, 0x4208, C_YELLOW, "RN");
            drawButtonRect(208, ey + 1, 32, 20, 0x4208, C_RED,    "DEL");
        }
    }

    // Nav bar row 1
    int navY = listY + GALLERY_PER_PAGE * 22 + 2;
    if (navY > 276) navY = 276;
    drawButtonRect(0,   navY, 48, 22, 0x4208, C_WHITE,  "<PRV");
    { const char *modeBtnLbl[] = {"IMAGES", "CYD"};
      uint16_t modeBtnBg = galleryMode > 0 ? C_CYAN : 0x4208;
      uint16_t modeBtnFg = galleryMode > 0 ? C_BLACK : C_CYAN;
      drawButtonRect(48, navY, 72, 22, modeBtnBg, modeBtnFg, modeBtnLbl[galleryMode]); }
    drawButtonRect(120, navY, 48, 22, 0x4208, C_WHITE,  "NXT>");
    drawButtonRect(168, navY, 72, 22, 0x4208, C_ORANGE, "BACK");

    // Nav bar row 2 — batch ops
    int nav2Y = navY + 24;
    drawButtonRect(0,   nav2Y, 80, 22, gallerySelMode ? C_YELLOW : 0x4208,
                   gallerySelMode ? C_BLACK : C_WHITE, gallerySelMode ? "CANCEL" : "SELECT");
    if (gallerySelMode) {
        drawButtonRect(80,  nav2Y, 80, 22, 0x4208, C_GREEN,  "SEL ALL");
        drawButtonRect(160, nav2Y, 80, 22, C_RED,   C_WHITE,  "DEL SEL");
    }
}

void handleGalleryTouch(int tx, int ty) {
    int listY = 14;
    int navY = listY + GALLERY_PER_PAGE * 22 + 2;
    if (navY > 276) navY = 276;
    int nav2Y = navY + 24;

    // Nav bar row 1
    if (rectHit(tx, ty, 168, navY, 72, 22)) { galleryClearSel(); drawMainScreen(); return; }  // BACK
    if (rectHit(tx, ty, 0, navY, 48, 22)) {
        if (galleryPage > 0) { galleryPage--; drawGalleryScreen(); }
        return;
    }
    if (rectHit(tx, ty, 120, navY, 48, 22)) {
        if ((galleryPage + 1) * GALLERY_PER_PAGE < galleryCount) { galleryPage++; drawGalleryScreen(); }
        return;
    }
    if (rectHit(tx, ty, 48, navY, 72, 22)) {
        galleryMode = (galleryMode + 1) % 2;
        galleryPage = 0;
        galleryClearSel();
        if (galleryMode == 0) scanFiles();
        else scanCYDs();
        drawGalleryScreen();
        return;
    }

    // Nav bar row 2 — batch ops
    if (rectHit(tx, ty, 0, nav2Y, 80, 22)) {
        if (gallerySelMode) { galleryClearSel(); } else { gallerySelMode = true; }
        drawGalleryScreen();
        return;
    }
    if (gallerySelMode && rectHit(tx, ty, 80, nav2Y, 80, 22)) {
        // SELECT ALL
        for (int i = 0; i < galleryCount; i++) gallerySel[i] = true;
        drawGalleryScreen();
        return;
    }
    if (gallerySelMode && rectHit(tx, ty, 160, nav2Y, 80, 22)) {
        // DEL SELECTED — count and confirm
        int cnt = 0;
        for (int i = 0; i < galleryCount; i++) if (gallerySel[i]) cnt++;
        if (cnt > 0) {
            confirmAction = CA_BATCH_DEL;
            char msg[40]; snprintf(msg, 40, "Delete %d selected?", cnt);
            drawConfirmScreen(msg);
        }
        return;
    }

    // List entries
    int startIdx = galleryPage * GALLERY_PER_PAGE;
    for (int i = 0; i < GALLERY_PER_PAGE && startIdx + i < galleryCount; i++) {
        int ey = listY + i * 22;
        int idx = startIdx + i;

        if (gallerySelMode) {
            // Tap anywhere on row toggles selection
            if (rectHit(tx, ty, 0, ey, SCREEN_W, 22)) {
                gallerySel[idx] = !gallerySel[idx];
                drawGalleryScreen();
                return;
            }
        } else {
            // LD button
            if (rectHit(tx, ty, 144, ey + 1, 32, 20)) {
                if (galleryMode == 1) {
                    loadCYD(idx);
                    if (animFrameCount > 1) drawAnimScreen();
                    else drawMainScreen();
                } else {
                    loadFile(idx);
                    drawMainScreen();
                }
                return;
            }
            // RN (rename) button
            if (rectHit(tx, ty, 176, ey + 1, 32, 20)) {
                renameIdx = idx;
                strncpy(renameBuf, galleryFiles[idx], 15);
                renameBuf[15] = '\0';
                // Strip .BMP extension for editing if it's an image
                if (galleryMode == 0) {  // single file rename for images
                    int ln = strlen(renameBuf);
                    if (ln > 4 && strcasecmp(renameBuf + ln - 4, ".BMP") == 0) renameBuf[ln-4] = '\0';
                }
                renameCursor = strlen(renameBuf);
                drawRenameScreen();
                return;
            }
            // DEL button
            if (rectHit(tx, ty, 208, ey + 1, 32, 20)) {
                confirmFileIdx = idx;
                char msg[40];
                {
                    confirmAction = CA_DEL_FILE;
                    snprintf(msg, 40, "Delete %s?", galleryFiles[idx]);
                }
                drawConfirmScreen(msg);
                return;
            }
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
            case CA_CLR:
                undoPush();
                clearCanvas(C_WHITE);
                break;
            case CA_DEL_FILE:
                if (sdOK && confirmFileIdx >= 0 && confirmFileIdx < galleryCount) {
                    char path[20];
                    snprintf(path, 20, "/%s", galleryFiles[confirmFileIdx]);
                    SD.remove(path);
                    countFiles();
                    updateStatusCounts();
                }
                break;
            case CA_DEL_ANIM:
                // Delete all ANMnnn_ff.BMP frames for this sequence
                if (sdOK && confirmFileIdx >= 0 && confirmFileIdx < galleryCount) {
                    char path[20];
                    for (int ff = 0; ff < MAX_FRAMES; ff++) {
                        snprintf(path, 20, "/%s_%02d.BMP", galleryFiles[confirmFileIdx], ff);
                        SD.remove(path);  // silently ignores if not found
                    }
                    countFiles();
                    updateStatusCounts();
                }
                break;
            case CA_BATCH_DEL:
                // Delete all selected items
                if (sdOK) {
                    for (int i = 0; i < galleryCount; i++) {
                        if (!gallerySel[i]) continue;
                        {
                            char path[20];
                            snprintf(path, 20, "/%s", galleryFiles[i]);
                            SD.remove(path);
                        }
                    }
                    countFiles();
                    updateStatusCounts();
                }
                break;
        }
    }

    if ((confirmAction == CA_DEL_FILE || confirmAction == CA_BATCH_DEL) && yes) {
        galleryPage = 0;
        gallerySelMode = false;
        if (galleryMode == 0) scanFiles(); else scanCYDs();
        drawGalleryScreen();
    } else {
        drawMainScreen();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Rename Screen — simple on-screen keyboard for renaming files/anims
// ─────────────────────────────────────────────────────────────────────────────
static const char *kbRows[] = {
    "ABCDEFGHIJ",
    "KLMNOPQRST",
    "UVWXYZ0123",
    "456789_-  "
};
#define KB_ROWS 4
#define KB_COLS 10
#define KB_KEY_W 24
#define KB_KEY_H 28
#define KB_START_Y 100

void drawRenameScreen() {
    currentScreen = SCR_RENAME;
    tft.fillScreen(0x1082);
    tft.setTextColor(C_WHITE, 0x1082);
    tft.setTextSize(1);
    tft.setCursor(4, 4);
    { const char *rnLbl[] = {"RENAME IMAGE","RENAME CYD"}; tft.print(rnLbl[galleryMode]); }

    // Current name display
    tft.fillRect(10, 30, 220, 30, 0x2104);
    tft.drawRect(10, 30, 220, 30, C_YELLOW);
    tft.setTextColor(C_WHITE, 0x2104);
    tft.setTextSize(2);
    tft.setCursor(14, 37);
    tft.print(renameBuf);
    // Cursor indicator
    int curX = 14 + renameCursor * 12;
    tft.drawFastVLine(curX, 35, 20, C_YELLOW);
    tft.setTextSize(1);

    // Original name
    tft.setTextColor(C_GRAY, 0x1082);
    tft.setCursor(10, 68);
    tft.printf("Was: %s", galleryFiles[renameIdx]);

    // Keyboard
    for (int r = 0; r < KB_ROWS; r++) {
        int y = KB_START_Y + r * KB_KEY_H;
        for (int c = 0; c < KB_COLS; c++) {
            char ch = kbRows[r][c];
            if (ch == ' ') continue;
            int x = c * KB_KEY_W;
            char lbl[2] = {ch, 0};
            drawButtonRect(x, y, KB_KEY_W, KB_KEY_H, 0x4208, C_WHITE, lbl);
        }
    }

    // Action buttons
    int abY = KB_START_Y + KB_ROWS * KB_KEY_H + 4;
    drawButtonRect(0,   abY, 60, 28, 0x4208, C_RED,    "BKSP");
    drawButtonRect(60,  abY, 60, 28, C_GREEN, C_BLACK,  "OK");
    drawButtonRect(120, abY, 60, 28, 0x4208, C_WHITE,   "CANCEL");
    drawButtonRect(180, abY, 60, 28, 0x4208, C_ORANGE,  "CLR");
}

void handleRenameTouch(int tx, int ty) {
    int abY = KB_START_Y + KB_ROWS * KB_KEY_H + 4;

    // Keyboard
    for (int r = 0; r < KB_ROWS; r++) {
        int y = KB_START_Y + r * KB_KEY_H;
        if (ty >= y && ty < y + KB_KEY_H) {
            int c = tx / KB_KEY_W;
            if (c >= 0 && c < KB_COLS) {
                char ch = kbRows[r][c];
                if (ch != ' ' && renameCursor < 11) {  // max 11 chars for 8.3 name safety
                    renameBuf[renameCursor++] = ch;
                    renameBuf[renameCursor] = '\0';
                    drawRenameScreen();
                }
            }
            return;
        }
    }

    // Action buttons
    if (rectHit(tx, ty, 0, abY, 60, 28)) {
        // BKSP
        if (renameCursor > 0) { renameBuf[--renameCursor] = '\0'; drawRenameScreen(); }
        return;
    }
    if (rectHit(tx, ty, 180, abY, 60, 28)) {
        // CLR
        renameBuf[0] = '\0'; renameCursor = 0; drawRenameScreen();
        return;
    }
    if (rectHit(tx, ty, 120, abY, 60, 28)) {
        // CANCEL
        galleryPage = 0;
        if (galleryMode == 0) scanFiles(); else scanCYDs();
        drawGalleryScreen();
        return;
    }
    if (rectHit(tx, ty, 60, abY, 60, 28)) {
        // OK — perform rename
        if (renameCursor == 0) return;  // can't rename to empty
        if (sdOK && renameIdx >= 0 && renameIdx < galleryCount) {
            if (0) {
                // (old anim rename removed)
                // renameBuf is the new prefix (up to 6 chars)
                char newPfx[7]; strncpy(newPfx, renameBuf, 6); newPfx[6] = '\0';
                for (int ff = 0; ff < MAX_FRAMES; ff++) {
                    char oldP[20], newP[20];
                    snprintf(oldP, 20, "/%s_%02d.BMP", galleryFiles[renameIdx], ff);
                    snprintf(newP, 20, "/%s_%02d.BMP", newPfx, ff);
                    if (SD.exists(oldP)) SD.rename(oldP, newP);
                }
            } else {
                // Rename PAINT*.BMP → newname.BMP
                char oldP[20], newP[20];
                snprintf(oldP, 20, "/%s", galleryFiles[renameIdx]);
                snprintf(newP, 20, "/%s.BMP", renameBuf);
                if (SD.exists(oldP)) SD.rename(oldP, newP);
            }
            updateStatusCounts();
        }
        galleryPage = 0;
        if (galleryMode == 0) scanFiles(); else scanCYDs();
        drawGalleryScreen();
        return;
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
    updateStatusCounts();
    setStatus("Saved OK");
}

void loadFile(int idx) {
    if (!sdOK || idx < 0 || idx >= galleryCount) return;
    undoClearAll(); redoClearAll();
    char path[20];
    snprintf(path, 20, "/%s", galleryFiles[idx]);
    File f = SD.open(path);
    if (!f) return;

    // Read and validate BMP header
    uint8_t hdr[54];
    if (f.read(hdr, 54) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        f.close(); setStatus("Not a valid BMP"); return;
    }
    uint32_t dataOffset = hdr[10] | (hdr[11]<<8) | (hdr[12]<<16) | (hdr[13]<<24);
    int32_t bmpW = (int32_t)(hdr[18] | (hdr[19]<<8) | (hdr[20]<<16) | (hdr[21]<<24));
    int32_t bmpH = (int32_t)(hdr[22] | (hdr[23]<<8) | (hdr[24]<<16) | (hdr[25]<<24));
    uint16_t bpp = hdr[28] | (hdr[29]<<8);

    if (bpp != 24) {
        f.close(); setStatus("Need 24-bit BMP"); return;
    }
    if (bmpW != CANVAS_W || abs(bmpH) != CANVAS_H) {
        f.close();
        char msg[32]; snprintf(msg, 32, "Need %dx%d BMP", CANVAS_W, CANVAS_H);
        setStatus(msg); return;
    }

    // BMP row stride is padded to 4-byte boundary
    uint32_t rowStride = ((bmpW * 3) + 3) & ~3;
    bool bottomUp = (bmpH > 0);
    f.seek(dataOffset);

    for (int i = 0; i < CANVAS_H; i++) {
        int row = bottomUp ? (CANVAS_H - 1 - i) : i;
        if (f.read(rowBuf, rowStride) != (int)rowStride) break;
        for (int col = 0; col < CANVAS_W; col++) {
            uint8_t b = rowBuf[col * 3 + 0];
            uint8_t g = rowBuf[col * 3 + 1];
            uint8_t r = rowBuf[col * 3 + 2];
            canvasSet(col, row, rgb888to565(r, g, b));
        }
    }
    f.close();
    setStatus("Image loaded");
}

// ─────────────────────────────────────────────────────────────────────────────

// rleCompress — if dst is nullptr, dry-run: just count and return byte size.
// Otherwise encode into dst (must be at least the dry-run size).
// Returns 0 only if dstCap is too small (never happens in dry-run mode).
uint32_t rleCompress(const uint16_t *src, uint8_t *dst, uint32_t dstCap) {
    uint32_t n = CANVAS_W * CANVAS_H, out = 0, i = 0;
    while (i < n) {
        uint16_t pix = src[i];
        uint8_t cnt = 1;
        while (cnt < 255 && i + cnt < n && src[i + cnt] == pix) cnt++;
        if (dst) {
            if (out + 3 > dstCap) return 0;  // overflow (only possible if cap too small)
            dst[out]   = cnt;
            dst[out+1] = (uint8_t)(pix & 0xFF);
            dst[out+2] = (uint8_t)(pix >> 8);
        }
        out += 3;
        i += cnt;
    }
    return out;
}

void rleDecompress(const uint8_t *src, uint32_t srcLen, uint16_t *dst) {
    uint32_t out = 0, total = CANVAS_W * CANVAS_H;
    for (uint32_t i = 0; i + 2 < srcLen && out < total; i += 3) {
        uint8_t  cnt = src[i];
        uint16_t pix = (uint16_t)src[i+1] | ((uint16_t)src[i+2] << 8);
        uint32_t end = min((uint32_t)(out + cnt), total);
        while (out < end) dst[out++] = pix;
    }
    while (out < (uint32_t)(CANVAS_W * CANVAS_H)) dst[out++] = C_WHITE;
}

// ─────────────────────────────────────────────────────────────────────────────
// Animation System
// ─────────────────────────────────────────────────────────────────────────────

bool animCompressAndStore(int slot) {
    if (slot < 0 || slot >= MAX_FRAMES) return false;

    // Dry-run: count exact compressed size without allocating anything
    uint32_t sz = rleCompress(canvas, nullptr, 0);
    if (sz == 0 || sz > ANIM_MAX_RLE_BYTES) {
        Serial.printf("compress[%d]: size %lu exceeds cap\n", slot, (unsigned long)sz);
        return false;
    }

    // Allocate exactly the right size (no oversized temp buffer)
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) {
        Serial.printf("compress[%d]: malloc(%lu) failed, heap=%d\n",
                      slot, (unsigned long)sz, (int)ESP.getFreeHeap());
        return false;
    }

    // Free old data ONLY after we know the new alloc succeeded
    if (animStore[slot].data) { free(animStore[slot].data); animStore[slot].data = nullptr; }

    rleCompress(canvas, buf, sz);
    animStore[slot].data = buf;
    animStore[slot].size = sz;
    Serial.printf("compress[%d]: %lu bytes, heap=%d\n", slot, (unsigned long)sz, (int)ESP.getFreeHeap());
    return true;
}

void animLoadFromStore(int slot) {
    if (slot < 0 || slot >= MAX_FRAMES) return;
    if (!animStore[slot].data) {
        for (int p = 0; p < CANVAS_W * CANVAS_H; p++) canvas[p] = C_WHITE;
    } else {
        rleDecompress(animStore[slot].data, animStore[slot].size, canvas);
    }
}

void animCommitFrame() {
    // Compress canvas into current frame's slot so it's safely stored
    // (called before entering SCR_ANIM so all frames are in animStore)
    if (animFrameCount > 1 || animCurFrame > 0) animCompressAndStore(animCurFrame);
}

void animSyncToFrame(int idx) {
    if (idx < 0 || idx >= animFrameCount || idx == animCurFrame) return;
    // Compress current frame — if this fails, abort (current frame stays live, nothing lost)
    if (!animCompressAndStore(animCurFrame)) {
        // Show brief error and stay on current frame
        tft.fillRect(40, 274, 160, 22, C_RED);
        tft.setTextColor(C_WHITE, C_RED); tft.setTextSize(1);
        tft.setCursor(44, 281); tft.print("SAVE FAIL - STAY");
        delay(800);
        return;
    }
    // Load target frame into canvas
    animLoadFromStore(idx);
    // Target slot data is now live in canvas — free the store copy to reclaim memory
    if (animStore[idx].data) { free(animStore[idx].data); animStore[idx].data = nullptr; animStore[idx].size = 0; }
    animCurFrame = idx;
}

void animAddFrame() {
    if (animFrameCount >= MAX_FRAMES) {
        tft.fillRect(80, 274, 80, 22, C_RED);
        tft.setTextColor(C_WHITE, C_RED); tft.setTextSize(1);
        tft.setCursor(84, 281); tft.print("MAX FRAMES"); delay(800); return;
    }
    if (!animCompressAndStore(animCurFrame)) {
        tft.fillRect(60, 274, 120, 22, C_RED);
        tft.setTextColor(C_WHITE, C_RED); tft.setTextSize(1);
        tft.setCursor(64, 281); tft.print("COMPRESS FAIL"); delay(800); return;
    }
    int insertAt = animCurFrame + 1;
    // Shift slots upward
    for (int i = animFrameCount; i > insertAt; i--) animStore[i] = animStore[i-1];
    // Copy compressed current frame into insertAt
    uint32_t srcSz = animStore[animCurFrame].size;
    animStore[insertAt].data = nullptr; animStore[insertAt].size = 0;
    if (animStore[animCurFrame].data && srcSz > 0) {
        uint8_t *copy = (uint8_t *)malloc(srcSz);
        if (copy) { memcpy(copy, animStore[animCurFrame].data, srcSz); animStore[insertAt].data = copy; animStore[insertAt].size = srcSz; }
    }
    animFrameCount++;
    // Load the new frame (which is a copy of the original) into canvas
    animLoadFromStore(insertAt);
    if (animStore[insertAt].data) { free(animStore[insertAt].data); animStore[insertAt].data = nullptr; animStore[insertAt].size = 0; }
    animCurFrame = insertAt;
    Serial.printf("addFrame: inserted at %d, count=%d, heap=%d\n", insertAt, animFrameCount, (int)ESP.getFreeHeap());
}

void animDeleteFrame() {
    if (animFrameCount <= 1) { clearCanvas(C_WHITE); return; }
    // Discard current frame (live in canvas — just abandon it)
    if (animStore[animCurFrame].data) { free(animStore[animCurFrame].data); animStore[animCurFrame].data = nullptr; animStore[animCurFrame].size = 0; }
    for (int i = animCurFrame; i < animFrameCount - 1; i++) animStore[i] = animStore[i+1];
    animStore[animFrameCount - 1].data = nullptr; animStore[animFrameCount - 1].size = 0;
    animFrameCount--;
    if (animCurFrame >= animFrameCount) animCurFrame = animFrameCount - 1;
    animLoadFromStore(animCurFrame);
    if (animStore[animCurFrame].data) { free(animStore[animCurFrame].data); animStore[animCurFrame].data = nullptr; animStore[animCurFrame].size = 0; }
}

void animMoveFrame(int from, int to) {
    if (from == to || from < 0 || from >= animFrameCount || to < 0 || to >= animFrameCount) return;
    // The current live frame (animCurFrame) is in canvas[], not in animStore.
    // We need to compress it first so all slots have data.
    animCompressAndStore(animCurFrame);

    // Extract the slot we're moving
    RleFrame moving = animStore[from];

    if (from < to) {
        // Shift everything between from+1..to down by one
        for (int i = from; i < to; i++) animStore[i] = animStore[i + 1];
    } else {
        // Shift everything between to..from-1 up by one
        for (int i = from; i > to; i--) animStore[i] = animStore[i - 1];
    }
    animStore[to] = moving;

    // Reload the frame that's now at 'to' position as the live frame
    animCurFrame = to;
    animLoadFromStore(animCurFrame);
    if (animStore[animCurFrame].data) { free(animStore[animCurFrame].data); animStore[animCurFrame].data = nullptr; animStore[animCurFrame].size = 0; }
}

void animSwapFrames(int a, int b) {
    if (a == b || a < 0 || a >= animFrameCount || b < 0 || b >= animFrameCount) return;
    // Compress live frame so all slots have data
    animCompressAndStore(animCurFrame);

    // Swap the RLE store entries
    RleFrame tmp = animStore[a];
    animStore[a] = animStore[b];
    animStore[b] = tmp;

    // Stay on whichever slot the current frame moved to
    if (animCurFrame == a) animCurFrame = b;
    else if (animCurFrame == b) animCurFrame = a;

    // Reload the live frame
    animLoadFromStore(animCurFrame);
    if (animStore[animCurFrame].data) { free(animStore[animCurFrame].data); animStore[animCurFrame].data = nullptr; animStore[animCurFrame].size = 0; }
}

void animDrawThumb(int fi, int sx, int sy, int tw, int th, bool hi) {
    if (fi < 0 || fi >= animFrameCount) { tft.fillRect(sx, sy, tw, th, 0x2104); return; }
    // Simple numbered box — no canvas preview
    uint16_t bg = hi ? 0x3186 : 0x2104;
    uint16_t border = hi ? C_YELLOW : C_DKGRAY;
    tft.fillRect(sx, sy, tw, th, bg);
    tft.drawRect(sx, sy, tw, th, border);
    if (hi) tft.drawRect(sx+1, sy+1, tw-2, th-2, border);
    // Frame number centred
    tft.setTextColor(hi ? C_YELLOW : C_GRAY, bg);
    tft.setTextSize(1);
    char num[4]; snprintf(num, 4, "%d", fi + 1);
    int tw2 = strlen(num) * 6;
    tft.setCursor(sx + (tw - tw2) / 2, sy + (th - 8) / 2);
    tft.print(num);
}

void animDrawFilmstrip() {
    tft.fillRect(0, ANIM_STRIP_Y, SCREEN_W, ANIM_THUMB_H + 4, 0x1082);
    int maxV = SCREEN_W / (ANIM_THUMB_W + 2);
    if (animCurFrame < animStripScroll) animStripScroll = animCurFrame;
    if (animCurFrame >= animStripScroll + maxV) animStripScroll = animCurFrame - maxV + 1;
    if (animStripScroll < 0) animStripScroll = 0;
    for (int v = 0; v < maxV; v++) {
        int fi = animStripScroll + v;
        if (fi >= animFrameCount) break;
        int sx = v * (ANIM_THUMB_W + 2) + 1;
        bool cur = (fi == animCurFrame);
        bool isSrc = (animOp != AOP_NONE && fi == animOpSrcFrame);
        animDrawThumb(fi, sx, ANIM_STRIP_Y + 2, ANIM_THUMB_W, ANIM_THUMB_H, cur || isSrc);
        // Draw colored border for source frame in MOVE/SWAP mode
        if (isSrc && !cur) {
            uint16_t bdrCol = (animOp == AOP_MOVE) ? C_YELLOW : C_MAGENTA;
            tft.drawRect(sx, ANIM_STRIP_Y + 2, ANIM_THUMB_W, ANIM_THUMB_H, bdrCol);
        }
    }
}

void drawAnimScreen() {
    // Note: do NOT call animStop() here — this is called mid-playback to refresh UI
    currentScreen = SCR_ANIM;
    tft.fillScreen(0x1082);
    tft.setTextColor(C_WHITE, 0x1082); tft.setTextSize(1);
    tft.setCursor(2, 4);
    tft.printf("ANIM F:%d/%d  %dfps", animCurFrame + 1, animFrameCount, animFPS);
    // Show MOVE/SWAP status if active
    if (animOp == AOP_MOVE) {
        tft.setTextColor(C_YELLOW, 0x1082);
        tft.setCursor(150, 4); tft.printf("MOV:%d", animOpSrcFrame + 1);
    } else if (animOp == AOP_SWAP) {
        tft.setTextColor(C_MAGENTA, 0x1082);
        tft.setCursor(150, 4); tft.printf("SWP:%d", animOpSrcFrame + 1);
    }

    // Canvas preview
    tft.startWrite();
    tft.setAddrWindow(CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H);
    tft.pushPixels(canvas, CANVAS_W * CANVAS_H);
    tft.endWrite();
    tft.drawRect(CANVAS_X-1, CANVAS_Y-1, CANVAS_W+2, CANVAS_H+2, C_WHITE);
    tft.fillRect(0, CANVAS_Y, CANVAS_X, CANVAS_H, 0x1082);
    tft.fillRect(220, CANVAS_Y, 20, CANVAS_H, 0x1082);
    animDrawFilmstrip();

    // Row 1 (y=248): nav + frame add/remove
    int r1 = 248;
    drawButtonRect(0,   r1, 34, 18, 0x4208, C_WHITE,  "|<");
    drawButtonRect(34,  r1, 34, 18, 0x4208, C_WHITE,  "<F");
    drawButtonRect(68,  r1, 34, 18, 0x4208, C_WHITE,  "F>");
    drawButtonRect(102, r1, 34, 18, 0x4208, C_WHITE,  ">|");
    drawButtonRect(136, r1, 36, 18, 0x4208, C_GREEN,  "+FRM");
    drawButtonRect(172, r1, 34, 18, 0x4208, C_RED,    "-FRM");
    // MOVE button
    { uint16_t bg = (animOp == AOP_MOVE) ? C_YELLOW : 0x4208;
      uint16_t fg = (animOp == AOP_MOVE) ? C_BLACK  : C_YELLOW;
      drawButtonRect(206, r1, 34, 18, bg, fg, "MOV"); }

    // Row 2 (y=268): SWAP + playback + fps
    int r2 = 268;
    { uint16_t bg = (animOp == AOP_SWAP) ? C_MAGENTA : 0x4208;
      uint16_t fg = (animOp == AOP_SWAP) ? C_BLACK   : C_MAGENTA;
      drawButtonRect(0, r2, 34, 18, bg, fg, "SWP"); }
    drawButtonRect(34,  r2, 40, 18, animPlaying ? C_GREEN : 0x4208, C_BLACK, animPlaying ? "STOP" : "PLAY");
    drawButtonRect(74,  r2, 20, 18, 0x4208, C_WHITE, "-");
    tft.fillRect(94, r2, 22, 18, 0x2104);
    tft.setTextColor(C_WHITE, 0x2104); tft.setTextSize(1);
    tft.setCursor(96, r2 + 5); tft.print(animFPS);
    drawButtonRect(116, r2, 20, 18, 0x4208, C_WHITE,  "+");
    // v24: ping-pong toggle
    drawButtonRect(136, r2, 34, 18, animPingPong ? C_CYAN : 0x4208,
                   animPingPong ? C_BLACK : C_CYAN, "PP");
    drawButtonRect(170, r2, 34, 18, 0x4208, C_YELLOW, "DRAW");
    drawButtonRect(204, r2, 36, 18, 0x4208, C_ORANGE, "SAVE");

    // Row 3 (y=288): help text for active ops
    int r3 = 288;
    if (animOp != AOP_NONE) {
        drawButtonRect(0, r3, 240, 14, 0x2104, C_WHITE,
            animOp == AOP_MOVE ? "Tap filmstrip dest to MOVE, or tap MOV to cancel"
                               : "Tap filmstrip frame to SWAP, or tap SWP to cancel");
    }
}

void handleAnimTouch(int tx, int ty) {
    int r1 = 248, r2 = 268;

    // During playback, only STOP responds
    if (animPlaying && !rectHit(tx, ty, 34, r2, 40, 18)) {
        animStop(); drawAnimScreen(); return;
    }

    // Filmstrip tap — normal nav, OR destination for MOVE/SWAP
    if (rectHit(tx, ty, 0, ANIM_STRIP_Y, SCREEN_W, ANIM_THUMB_H + 4)) {
        int fi = animStripScroll + tx / (ANIM_THUMB_W + 2);
        if (fi < 0 || fi >= animFrameCount) return;

        if (animOp == AOP_MOVE && animOpSrcFrame >= 0) {
            // Complete MOVE: move source frame to position fi
            animMoveFrame(animOpSrcFrame, fi);
            animOp = AOP_NONE; animOpSrcFrame = -1;
            drawAnimScreen();
            return;
        }
        if (animOp == AOP_SWAP && animOpSrcFrame >= 0) {
            // Complete SWAP
            if (fi != animOpSrcFrame) {
                animSwapFrames(animOpSrcFrame, fi);
            }
            animOp = AOP_NONE; animOpSrcFrame = -1;
            drawAnimScreen();
            return;
        }

        // Normal filmstrip nav
        if (fi != animCurFrame) { animSyncToFrame(fi); drawAnimScreen(); }
        return;
    }

    // Row 1: nav + frame ops + MOV
    if (rectHit(tx, ty, 0, r1, 240, 18)) {
        if      (rectHit(tx,ty,0,  r1,34,18)) { animOp=AOP_NONE; animSyncToFrame(0);                                          drawAnimScreen(); }
        else if (rectHit(tx,ty,34, r1,34,18)) { animOp=AOP_NONE; if(animCurFrame>0) animSyncToFrame(animCurFrame-1);          drawAnimScreen(); }
        else if (rectHit(tx,ty,68, r1,34,18)) { animOp=AOP_NONE; if(animCurFrame<animFrameCount-1) animSyncToFrame(animCurFrame+1); drawAnimScreen(); }
        else if (rectHit(tx,ty,102,r1,34,18)) { animOp=AOP_NONE; animSyncToFrame(animFrameCount-1);                           drawAnimScreen(); }
        else if (rectHit(tx,ty,136,r1,36,18)) { animOp=AOP_NONE; animAddFrame();    drawAnimScreen(); }
        // Note: DUP is handled via +FRM (it already copies current frame)
        else if (rectHit(tx,ty,172,r1,34,18)) { animOp=AOP_NONE; animDeleteFrame(); drawAnimScreen(); }
        else if (rectHit(tx,ty,206,r1,34,18)) {
            // MOV button — toggle move mode
            if (animOp == AOP_MOVE) {
                animOp = AOP_NONE; animOpSrcFrame = -1;
            } else {
                animOp = AOP_MOVE;
                animOpSrcFrame = animCurFrame;
            }
            drawAnimScreen();
        }
        return;
    }

    // Row 2: SWP + playback + controls
    if (rectHit(tx, ty, 0, r2, 240, 18)) {
        if (rectHit(tx,ty,0,r2,34,18)) {
            // SWP button — toggle swap mode
            if (animOp == AOP_SWAP) {
                animOp = AOP_NONE; animOpSrcFrame = -1;
            } else {
                animOp = AOP_SWAP;
                animOpSrcFrame = animCurFrame;
            }
            drawAnimScreen();
        }
        else if (rectHit(tx,ty,34,r2,40,18))  { animOp=AOP_NONE; if(animPlaying) animStop(); else animPlay(); drawAnimScreen(); }
        else if (rectHit(tx,ty,74,r2,20,18))  { if(animFPS>ANIM_FPS_MIN) animFPS--; drawAnimScreen(); }
        else if (rectHit(tx,ty,116,r2,20,18)) { if(animFPS<ANIM_FPS_MAX) animFPS++; drawAnimScreen(); }
        else if (rectHit(tx,ty,136,r2,34,18)) { animPingPong = !animPingPong; animPlayDir = 1; drawAnimScreen(); }
        else if (rectHit(tx,ty,170,r2,34,18)) { animOp=AOP_NONE; drawMainScreen(); }
        else if (rectHit(tx,ty,204,r2,36,18)) { animOp=AOP_NONE; saveCYD(); drawAnimScreen(); }
        return;
    }
}

void animPlay() {
    if (animFrameCount < 2) return;
    // Compress the live frame so ALL frames are in animStore for the playback loop
    animCompressAndStore(animCurFrame);
    // Playback loop reads directly from animStore without freeing slots,
    // so canvas just gets overwritten each tick. Start from frame 0.
    animCurFrame = 0;
    animLoadFromStore(0);
    animPlaying = true;
    // Set timestamp so first frame swap happens after 1 full interval
    animLastFrameTime = millis();
}

void animStop() { animPlaying = false; }
