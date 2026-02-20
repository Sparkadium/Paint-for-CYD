// CYD Paint v23 — ESP32-2432S028 (ILI9341 240x320, XPT2046, SD)
// v17: Mirror mode (MX/MY)
// v18: Animation editor (broken — full frame buffers OOM)
// v19: Animation with RLE compression — only 1 full buffer in RAM at a time
// v20: 2-tap zoom in / 1-tap zoom out; compact text-list gallery (12 per page)
// v21: 8× zoom; status bar; gallery batch-delete & rename; anim=image sequences
// v22: Undo/redo (RLE snapshots); numbered filmstrip boxes; MOVE/SWAP frame reorder
// v23: Selection rect + lasso tools; onion skin removed to free 80KB for selection buf
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
#define BOTTOM_Y   220   // bottom bar starts here (100 px tall)
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
    TOOL_SEL_RECT,
    TOOL_LASSO,
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
enum Screen { SCR_MAIN = 0, SCR_COLOR_PICK, SCR_GALLERY, SCR_CONFIRM, SCR_ANIM, SCR_RENAME };

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
int          animCount = 0;       // animation sequences

// Gallery state
int  galleryPage = 0;
char galleryFiles[128][16];     // image mode: filename e.g. "PAINT000.BMP"
                                // anim  mode: sequence prefix e.g. "ANM000"
int  galleryCount = 0;
bool galleryShowAnims = false;  // false=images, true=animations
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
#define TOUCH_DEBOUNCE_MS    80
#define CANVAS_DEBOUNCE_MS   120   // slightly longer — prevents double-fill on tap

// Smooth drawing state
int  lastDrawX = -1;   // previous canvas-space X (-1 = no previous point)
int  lastDrawY = -1;
bool wasDrawing = false;  // was the stylus on the canvas last frame?

// Mirror mode state
bool mirrorX = false;   // mirror horizontally (flip across vertical centre line)
bool mirrorY = false;   // mirror vertically   (flip across horizontal centre line)

// Zoom state
int    zoomLevel    = 0;       // 0=1× (no zoom), 1=2×, 2=4×, 3=8×
#define ZOOM_MAX_LEVEL 3
bool   zoomPending  = false;   // first tap done, waiting for second?
// Derived viewport (canvas coords visible on screen) — recomputed on zoom in/out
int    zoomVX = 0, zoomVY = 0; // top-left of visible region in canvas space
int    zoomVW = CANVAS_W;      // width  of visible region (CANVAS_W when not zoomed)
int    zoomVH = CANVAS_H;      // height of visible region

// Two-tap shape state
bool shapeHasOrigin = false;  // waiting for second tap?
int  shapeOriginX   = 0;
int  shapeOriginY   = 0;

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
void scanAnims();
void loadAnim(int idx);
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
void animSaveAll();
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
    for (int i = 0; i < MAX_FRAMES; i++) { animStore[i].data = nullptr; animStore[i].size = 0; }
    for (int i = 0; i < UNDO_MAX_SLOTS; i++) { undoStack[i].data = nullptr; undoStack[i].size = 0; redoStack[i].data = nullptr; redoStack[i].size = 0; }
    animFrameCount = 1;
    animCurFrame   = 0;

    // Init custom colors
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
        if (now - animLastFrameTime >= 1000UL / animFPS) {
            animLastFrameTime = now;
            int next = (animCurFrame + 1) % animFrameCount;
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
            tft.printf("ANIM F:%d/%d %dfps", animCurFrame + 1, animFrameCount, animFPS);
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
                setStatus("Tap destination");
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

        if (lastDrawX < 0) {
            undoPush();  // snapshot before first point of new stroke
            toolApply(cx, cy);
        } else {
            int dx = cx - lastDrawX, dy = cy - lastDrawY;
            if (dx*dx + dy*dy < ((zoomLevel > 0) ? 2 : 8)) return;
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
                setStatus("Tap destination");
            } else if (selPhase == SEL_PLACE) {
                selPlace(cx, cy);
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
                selPlace(cx, cy);
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

    size_t sz = selW * selH * sizeof(uint16_t);
    selBuf = (uint16_t *)malloc(sz);
    if (!selBuf) { setStatus("No mem for sel"); selCancel(); return; }

    // Copy pixels from canvas
    for (int y = 0; y < selH; y++) {
        for (int x = 0; x < selW; x++) {
            selBuf[y * selW + x] = canvas[(selY1 + y) * CANVAS_W + (selX1 + x)];
        }
    }
    // Clear source area on canvas
    for (int y = selY1; y <= selY2; y++)
        for (int x = selX1; x <= selX2; x++)
            canvas[y * CANVAS_W + x] = C_WHITE;
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
                canvas[(selY1 + y) * CANVAS_W + (selX1 + x)] = C_WHITE;
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
    drawStatusBar();
    pushCanvas();
    drawBottomBar();
    currentScreen = SCR_MAIN;
}

void drawStatusBar() {
    // Thin text row in the top gap (x=20..219, y=0..19) — between colour strip tops
    tft.fillRect(CANVAS_X, STATUS_Y, CANVAS_W, STATUS_H, C_DKGRAY);
    tft.setTextSize(1);
    if (statusMsg[0]) {
        tft.setTextColor(C_GREEN, C_DKGRAY);
        tft.setCursor(CANVAS_X + 2, STATUS_Y + 6);
        tft.print(statusMsg);
    } else {
        tft.setTextColor(C_GRAY, C_DKGRAY);
        tft.setCursor(CANVAS_X + 2, STATUS_Y + 6);
        tft.printf("I:%d A:%d U:%d %dKB", imgCount, animCount, undoCount, (int)(ESP.getFreeHeap()/1024));
    }
}

void setStatus(const char *msg) {
    strncpy(statusMsg, msg, 31); statusMsg[31] = '\0';
    statusExpiry = millis() + 2000;  // show for 2 seconds
    if (currentScreen == SCR_MAIN) drawStatusBar();
}

void updateStatusCounts() {
    imgCount = 0;
    animCount = 0;
    if (!sdOK) return;
    char seen[64][7]; int seenN = 0;
    File root = SD.open("/");
    while (true) {
        File f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            const char *nm = f.name();
            int len = strlen(nm);
            if (len > 4 && strcasecmp(nm + len - 4, ".bmp") == 0) {
                if (strncasecmp(nm, "PAINT", 5) == 0) imgCount++;
                else if (len == 13 && strncasecmp(nm, "ANM", 3) == 0 && nm[6] == '_') {
                    char pfx[7]; strncpy(pfx, nm, 6); pfx[6] = 0;
                    bool dup = false;
                    for (int s = 0; s < seenN; s++) if (strcasecmp(seen[s], pfx)==0) { dup=true; break; }
                    if (!dup && seenN < 64) { strncpy(seen[seenN], pfx, 7); seenN++; animCount++; }
                }
            }
        }
        f.close();
    }
    root.close();
}

void drawLeftStrip() {
    drawSwatchStrip(0, PRESET_COLORS, 12);
    uint16_t mxBg = mirrorX ? C_CYAN : 0x4208;
    uint16_t mxFg = mirrorX ? C_BLACK : C_WHITE;
    drawButtonRect(0, 200, 20, 20, mxBg, mxFg, "MX");
}

void drawRightStrip() {
    drawSwatchStrip(220, customColors, 12);
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
    const char *toolNames[TOOL_COUNT] = {"Pen","Eraser","Fill","Spray","Line","Rect","Ellipse","Eyedrop","SelRect","Lasso"};
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
    drawButtonRect(0,  ry, 28, 30, 0x4208, C_WHITE, "-Sz");
    drawButtonRect(28, ry, 28, 30, 0x4208, C_WHITE, "+Sz");
    tft.fillRect(56, ry, 16, 30, 0x2104);
    tft.setTextColor(C_WHITE, 0x2104); tft.setTextSize(1);
    tft.setCursor(58, ry + 11); tft.print(brushSize);
    drawButtonRect(72, ry, 52, 30, 0x4208, C_YELLOW, "COL");
    // Undo / Redo
    drawButtonRect(124, ry, 24, 30, undoCount > 0 ? 0x4208 : 0x2104,
                   undoCount > 0 ? C_WHITE : C_DKGRAY, "<-");
    drawButtonRect(148, ry, 24, 30, redoCount > 0 ? 0x4208 : 0x2104,
                   redoCount > 0 ? C_WHITE : C_DKGRAY, "->");
    // Zoom buttons
    { uint16_t ziBg = zoomPending ? C_YELLOW : ((zoomLevel > 0) ? C_CYAN : 0x4208);
      uint16_t ziFg = (zoomPending || (zoomLevel > 0)) ? C_BLACK : C_WHITE;
      char zlbl[4];
      if (zoomLevel == 0) strcpy(zlbl, "Z+");
      else snprintf(zlbl, 4, "%dX", 1 << zoomLevel);
      drawButtonRect(172, ry, 24, 30, ziBg, ziFg, zlbl); }
    drawButtonRect(196, ry, 24, 30, (zoomLevel > 0) ? C_ORANGE : 0x4208,
                   (zoomLevel > 0) ? C_BLACK : C_DKGRAY, "Z-");
    { char lbl[10]; snprintf(lbl, 10, "A:%d", animFrameCount);
      drawButtonRect(220, ry, 20, 30, 0x4208, C_LIME, lbl); }

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
            shapeHasOrigin = false; selCancel();
            drawBottomBar();
            return;
        }
        // [PEN] reset to pen
        if (rectHit(tx, ry, 36, 0, 36, 30)) {
            activeTool = TOOL_PEN;
            shapeHasOrigin = false; selCancel();
            drawBottomBar();
            return;
        }
        // [>] next tool
        if (rectHit(tx, ry, 72, 0, 36, 30)) {
            activeTool = (Tool)((activeTool + 1) % TOOL_COUNT);
            shapeHasOrigin = false; selCancel();
            drawBottomBar();
            return;
        }
        return;
    }

    // Row 1
    if (ry < 60) {
        int localY = ry - 30;
        if (rectHit(tx, localY, 0, 0, 28, 30)) {
            if (brushSize > 1) brushSize--;
            drawBottomBar(); return;
        }
        if (rectHit(tx, localY, 28, 0, 28, 30)) {
            if (brushSize < 8) brushSize++;
            drawBottomBar(); return;
        }
        if (rectHit(tx, localY, 72, 0, 52, 30)) {  // COL
            cpSlot = 0;
            uint8_t r, g, b; rgb565to888(activeColor, r, g, b);
            cpR = r; cpG = g; cpB = b;
            drawColorPickScreen(); return;
        }
        if (rectHit(tx, localY, 124, 0, 24, 30)) {  // UNDO
            undoPerform();
            drawBottomBar(); return;
        }
        if (rectHit(tx, localY, 148, 0, 24, 30)) {  // REDO
            redoPerform();
            drawBottomBar(); return;
        }
        if (rectHit(tx, localY, 172, 0, 24, 30)) {  // Z+ (zoom in)
            if (zoomLevel < ZOOM_MAX_LEVEL && !zoomPending) {
                zoomPending = true;
                drawBottomBar();
            } else if (zoomPending) {
                zoomPending = false;
                drawBottomBar();
            }
            return;
        }
        if (rectHit(tx, localY, 196, 0, 24, 30)) {  // Z- (zoom out)
            if ((zoomLevel > 0)) {
                zoomOut();
                pushCanvas();
                drawBottomBar();
            } else if (zoomPending) {
                zoomPending = false;
                drawBottomBar();
            }
            return;
        }
        if (rectHit(tx, localY, 220, 0, 20, 30)) {  // ANIM button
            animCommitFrame();
            drawAnimScreen(); return;
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
            galleryShowAnims = false;
            galleryPage = 0;
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
                // Only PAINT*.BMP (not ANM* frames)
                if (strncasecmp(nm, "PAINT", 5) == 0) {
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

// scanAnims — populate galleryFiles with unique ANM sequence prefixes (e.g. "ANM000")
void scanAnims() {
    galleryCount = 0;
    if (!sdOK) return;
    // Collect unique ANMnnn prefixes from ANMnnn_ff.BMP files
    char seen[64][7];  // up to 64 unique prefixes, "ANMnnn" + null
    int seenCount = 0;
    File root = SD.open("/");
    while (true) {
        File f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            const char *nm = f.name();
            int len = strlen(nm);
            // Match ANMnnn_ff.BMP — 14 chars total, prefix is first 6
            if (len == 13 && strncasecmp(nm, "ANM", 3) == 0 && nm[6] == '_') {
                char prefix[7];
                strncpy(prefix, nm, 6); prefix[6] = 0;
                // Check if we've already seen this prefix
                bool found = false;
                for (int s = 0; s < seenCount; s++) {
                    if (strcasecmp(seen[s], prefix) == 0) { found = true; break; }
                }
                if (!found && seenCount < 64) {
                    strncpy(seen[seenCount], prefix, 7);
                    seenCount++;
                }
            }
        }
        f.close();
    }
    root.close();
    // Sort prefixes alphabetically so they appear in order
    for (int i = 0; i < seenCount - 1; i++)
        for (int j = i + 1; j < seenCount; j++)
            if (strcasecmp(seen[i], seen[j]) > 0) { char tmp[7]; memcpy(tmp,seen[i],7); memcpy(seen[i],seen[j],7); memcpy(seen[j],tmp,7); }
    for (int i = 0; i < seenCount && i < 128; i++) {
        strncpy(galleryFiles[i], seen[i], 15);
        galleryFiles[i][15] = 0;
        galleryCount++;
    }
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
    const char *modeLabel = galleryShowAnims ? "ANIM" : "IMG";
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
            tft.setTextColor(galleryShowAnims ? C_CYAN : C_WHITE, rowBg);
            tft.setCursor(16, ey + 7);
        } else {
            tft.setTextColor(galleryShowAnims ? C_CYAN : C_WHITE, rowBg);
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
    drawButtonRect(48,  navY, 72, 22, galleryShowAnims ? C_CYAN : 0x4208,
                                    galleryShowAnims ? C_BLACK : C_CYAN,
                                    galleryShowAnims ? "ANIMS" : "IMAGES");
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
        galleryShowAnims = !galleryShowAnims;
        galleryPage = 0;
        galleryClearSel();
        if (galleryShowAnims) scanAnims(); else scanFiles();
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
                if (galleryShowAnims) {
                    loadAnim(idx);
                    drawAnimScreen();
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
                if (!galleryShowAnims) {
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
                if (galleryShowAnims) {
                    confirmAction = CA_DEL_ANIM;
                    snprintf(msg, 40, "Delete anim %s?", galleryFiles[idx]);
                } else {
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
                        if (galleryShowAnims) {
                            char path[20];
                            for (int ff = 0; ff < MAX_FRAMES; ff++) {
                                snprintf(path, 20, "/%s_%02d.BMP", galleryFiles[i], ff);
                                SD.remove(path);
                            }
                        } else {
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

    if ((confirmAction == CA_DEL_FILE || confirmAction == CA_DEL_ANIM || confirmAction == CA_BATCH_DEL) && yes) {
        galleryPage = 0;
        gallerySelMode = false;
        if (galleryShowAnims) scanAnims(); else scanFiles();
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
    tft.print(galleryShowAnims ? "RENAME ANIM" : "RENAME IMAGE");

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
        if (galleryShowAnims) scanAnims(); else scanFiles();
        drawGalleryScreen();
        return;
    }
    if (rectHit(tx, ty, 60, abY, 60, 28)) {
        // OK — perform rename
        if (renameCursor == 0) return;  // can't rename to empty
        if (sdOK && renameIdx >= 0 && renameIdx < galleryCount) {
            if (galleryShowAnims) {
                // Rename all ANMnnn_ff.BMP → NEWnnn_ff.BMP
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
        if (galleryShowAnims) scanAnims(); else scanFiles();
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

// loadAnim — load an animation sequence (ANMnnn) into animStore and go to anim screen
void loadAnim(int idx) {
    if (!sdOK || idx < 0 || idx >= galleryCount) return;
    const char *prefix = galleryFiles[idx];  // e.g. "ANM000"

    // Clear current animation state
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (animStore[i].data) { free(animStore[i].data); animStore[i].data = nullptr; animStore[i].size = 0; }
    }
    animFrameCount = 0;
    animCurFrame   = 0;
    animStripScroll = 0;

    uint32_t rowStride = CANVAS_W * 3;
    for (int ff = 0; ff < MAX_FRAMES; ff++) {
        char path[20];
        snprintf(path, 20, "/%s_%02d.BMP", prefix, ff);
        File f = SD.open(path);
        if (!f) break;  // no more frames

        // Read BMP into canvas temporarily, then compress into animStore[ff]
        f.seek(54);
        bool ok = true;
        for (int row = CANVAS_H - 1; row >= 0; row--) {
            if (f.read(rowBuf, rowStride) != (int)rowStride) { ok = false; break; }
            for (int col = 0; col < CANVAS_W; col++) {
                uint8_t b = rowBuf[col*3+0], g = rowBuf[col*3+1], r = rowBuf[col*3+2];
                canvas[row * CANVAS_W + col] = rgb888to565(r, g, b);
            }
        }
        f.close();
        if (!ok) break;

        if (!animCompressAndStore(ff)) break;  // OOM — stop here
        animFrameCount++;
    }

    if (animFrameCount == 0) {
        // Nothing loaded — start fresh
        clearCanvas(C_WHITE);
        animFrameCount = 1;
        animCurFrame = 0;
        return;
    }

    // Load frame 0 as the live frame
    animLoadFromStore(0);
    if (animStore[0].data) { free(animStore[0].data); animStore[0].data = nullptr; animStore[0].size = 0; }
    animCurFrame = 0;
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
    drawButtonRect(136, r2, 52, 18, 0x4208, C_YELLOW, "DRAW");
    drawButtonRect(188, r2, 52, 18, 0x4208, C_ORANGE, "SAVE");

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
        else if (rectHit(tx,ty,136,r2,52,18)) { animOp=AOP_NONE; drawMainScreen(); }
        else if (rectHit(tx,ty,188,r2,52,18)) { animOp=AOP_NONE; animSaveAll(); drawAnimScreen(); }
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

void animSaveAll() {
    if (!sdOK) return;
    char base[20]; int animNum = 0;
    for (; animNum < 1000; animNum++) {
        snprintf(base, 20, "/ANM%03d_00.BMP", animNum);
        if (!SD.exists(base)) break;
    }
    uint32_t rowStride = CANVAS_W * 3, dataSize = rowStride * CANVAS_H, fileSize = 54 + dataSize;

    // Compress the live frame before iterating so every slot has data
    animCompressAndStore(animCurFrame);

    for (int fi = 0; fi < animFrameCount; fi++) {
        // Decompress this frame into canvas (safe — we're just reading for save)
        if (animStore[fi].data) {
            rleDecompress(animStore[fi].data, animStore[fi].size, canvas);
        } else {
            // Slot is empty — shouldn't happen after the compress above, but handle it
            clearCanvas(C_WHITE);
        }

        char path[20]; snprintf(path, 20, "/ANM%03d_%02d.BMP", animNum, fi);
        File f = SD.open(path, FILE_WRITE);
        if (!f) continue;

        uint8_t hdr[54] = {0};
        hdr[0]='B'; hdr[1]='M';
        hdr[2]=fileSize&0xFF; hdr[3]=(fileSize>>8)&0xFF;
        hdr[4]=(fileSize>>16)&0xFF; hdr[5]=(fileSize>>24)&0xFF;
        hdr[10]=54; hdr[14]=40;
        hdr[18]=CANVAS_W&0xFF; hdr[19]=(CANVAS_W>>8)&0xFF;
        hdr[22]=CANVAS_H&0xFF; hdr[23]=(CANVAS_H>>8)&0xFF;
        hdr[26]=1; hdr[28]=24;
        hdr[34]=dataSize&0xFF; hdr[35]=(dataSize>>8)&0xFF;
        hdr[36]=(dataSize>>16)&0xFF; hdr[37]=(dataSize>>24)&0xFF;
        f.write(hdr, 54);

        for (int row = CANVAS_H-1; row >= 0; row--) {
            for (int col = 0; col < CANVAS_W; col++) {
                uint8_t r2, g2, b2;
                rgb565to888(canvas[row*CANVAS_W+col], r2, g2, b2);
                rowBuf[col*3]=b2; rowBuf[col*3+1]=g2; rowBuf[col*3+2]=r2;
            }
            f.write(rowBuf, rowStride);
        }
        f.close();
    }

    // Restore the live frame back into canvas
    animLoadFromStore(animCurFrame);
    // Free that slot again (it's live in canvas)
    if (animStore[animCurFrame].data) {
        free(animStore[animCurFrame].data);
        animStore[animCurFrame].data = nullptr;
        animStore[animCurFrame].size = 0;
    }

    countFiles();
    updateStatusCounts();
    setStatus("Anim saved OK");
}
