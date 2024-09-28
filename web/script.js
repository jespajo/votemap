/**
        @typedef {{
                scale:      number,
                rotate:     number,
                translateX: number,
                translateY: number
            }} Transform

        @typedef {{x: number, y: number}} Vec2

        @typedef {{x: number, y: number, width: number, height: number}} Rect

    A Box is a rectangle aligned with the XY axes. Its two points are its lower-left and upper-right corners, in that order.

        @typedef {[Vec2, Vec2]} Box

    When a box gets rotated, we need an extra two points to record all four corners in an unspecified order.

        @typedef {[Vec2, Vec2, Vec2, Vec2]} Box4
 */

const $ = document.querySelector.bind(document);
const $$ = document.querySelectorAll.bind(document);

const VERTEX_SHADER_TEXT = `
    precision highp float;

    uniform mat3 u_proj;
    uniform mat3 u_view;

    attribute vec2 v_position;
    attribute vec3 v_colour;

    varying vec3 f_colour;

    void main()
    {
        mat3 matrix = u_proj * u_view;
        vec3 position = matrix * vec3(v_position, 1.0);

        gl_Position = vec4(position.xy, 0.0, 1.0);
        f_colour = v_colour;
    }
`;

const FRAGMENT_SHADER_TEXT = `
    precision mediump float;

    varying vec3 f_colour;

    void main()
    {
        gl_FragColor = vec4(f_colour, 1.0);
    }
`;

/**
 * @type {(transform: Transform, vec2: Vec2) => Vec2}
 */
function xform(transform, vec2) {
    const {scale, rotate, translateX, translateY} = transform;
    const {x, y} = vec2;

    const sin = Math.sin(rotate);
    const cos = Math.cos(rotate);

    const x1 = scale*(x*cos + y*sin) + translateX;
    const y1 = scale*(y*cos - x*sin) + translateY;

    return {x: x1, y: y1};
}

/**
 * @type {(transform: Transform, vec2: Vec2) => Vec2}
 */
function inverseXform(transform, vec2) {
    const {scale, rotate, translateX, translateY} = transform;
    const {x, y} = vec2;

    const sin = Math.sin(rotate);
    const cos = Math.cos(rotate);

    // |Todo: Simpler way?!?!
    const det = scale*scale*(cos*cos + sin*sin);
    if (det == 0)  throw new Error();

    const x1 = x - translateX;
    const y1 = y - translateY;

    // |Cleanup: Redundant scale on numerator and denom.
    const x2 = (scale/det)*(x1*cos - y1*sin);
    const y2 = (scale/det)*(x1*sin + y1*cos);

    return {x: x2, y: y2};
}

/**
 * @type {(a: number, b: number, t: number) => number}
 */
function lerp(a, b, t) {
    return (1-t)*a + t*b;
}

function clone(object) {
    return JSON.parse(JSON.stringify(object));
}

/**
 * Return the transform (applied to the inner box) required to fit the inner box in the centre of the outer box.
 *
 * @type {(inner: Box, outer: Box) => Transform}
 */
function fitBox(inner, outer) {
    const innerWidth  = inner[1].x - inner[0].x;
    const innerHeight = inner[1].y - inner[0].y;
    const outerWidth  = outer[1].x - outer[0].x;
    const outerHeight = outer[1].y - outer[0].y;

    const innerRatio = innerWidth/innerHeight;
    const outerRatio = outerWidth/outerHeight;

    const rotate = 0;

    const scale = (innerRatio < outerRatio) ? outerHeight/innerHeight : outerWidth/innerWidth;

    const translateX = -scale*inner[0].x + (outerWidth - scale*innerWidth)/2;
    const translateY = -scale*inner[0].y + (outerHeight - scale*innerHeight)/2;

    return {scale, rotate, translateX, translateY};
}

/**
 * If one of the boxes contains the other, return the larger box (the actual object, not a copy of it).
 *
 * @type {(a: Box, b: Box) => Box}
 */
function combineBoxes(a, b) {
    if ((a[0].x <= b[0].x) && (a[0].y <= b[0].y) && (a[1].x >= b[1].x) && (a[1].y >= b[1].y)) {
        return a;
    }
    if ((b[0].x <= a[0].x) && (b[0].y <= a[0].y) && (b[1].x >= a[1].x) && (b[1].y >= a[1].y)) {
        return b;
    }

    const minX = (a[0].x < b[0].x) ? a[0].x : b[0].x;
    const minY = (a[0].y < b[0].y) ? a[0].y : b[0].y;
    const maxX = (a[1].x > b[1].x) ? a[1].x : b[1].x;
    const maxY = (a[1].y > b[1].y) ? a[1].y : b[1].y;

    return [{x: minX, y: minY}, {x: maxX, y: maxY}];
}

/**
 * Get the coordinates, in the map's coordinate reference system, of the four corners of the map (the portion of it that is displayed).
 *
 * @type {(width: number, height: number, transform: Transform) => Box4}
 */
function getMapCorners(width, height, transform) {
    /** @type Box4 */
    const box = [
        {x: 0,     y: 0},
        {x: 0,     y: height},
        {x: width, y: height},
        {x: width, y: 0}
    ];

    for (let i = 0; i < 4; i++)  box[i] = inverseXform(transform, box[i]);

    return box;
}

/**
 * Expand a 4-pointed box (i.e. one that has rotation) into its 2-point envelope.
 * In other words, find the rectangle that encloses the diamond.
 *
 * @type {(box: Box4) => Box}
 */
function getEnvelope(box) {
    let minX = box[0].x;
    let minY = box[0].y;
    let maxX = box[0].x;
    let maxY = box[0].y;

    for (let i = 1; i < 4; i++) {
        const {x, y} = box[i];

        if (x < minX)       minX = x;
        else if (x > maxX)  maxX = x;

        if (y < minY)       minY = y;
        else if (y > maxY)  maxY = y;
    }

    return [{x: minX, y: minY}, {x: maxX, y: maxY}];
}

/**
 * @type {(point: Vec2, rect: Rect) => boolean}
 */
function pointInRect(point, rect) {
    if (point.x < rect.x)                return false;
    if (point.y < rect.y)                return false;
    if (point.x > rect.x + rect.width)   return false;
    if (point.y > rect.y + rect.height)  return false;

    return true;
}

/**
 * @type {(rect: Rect, width: number) => [Rect, Rect]}
 */
function cutLeft(rect, width) {
    const leftRect  = clone(rect);
    const remainder = clone(rect);

    leftRect.width   = width;
    remainder.x     += width;
    remainder.width -= width;

    return [leftRect, remainder];
}

async function loadFonts() {
    const fonts = {
        //|Temporary: This is a convenient way to play with the fonts. Unfortunately the browser loads every font in this list independently, even if it shares a URL with another font. So at some point we'll either replace our keys with unique identifiers, or, since new FontFace() can take an ArrayBuffer, we could deduplicate the values and fetch them ourselves.
        "map-electorate":       "url(../fonts/RadioCanada.500.80.woff2)",
        "title":                "url(../fonts/RadioCanada.500.90.woff2)",
        "button-active":        "url(../fonts/RadioCanada.700.90.woff2)",
        "button-inactive":      "url(../fonts/RadioCanada.300.90.woff2)",
        "party-label":          "url(../fonts/RadioCanada.700.90.woff2)",
    };

    for (const name of Object.keys(fonts)) {
        const fontFace = new FontFace(name, fonts[name]);
        const f = await fontFace.load();
        document.fonts.add(f);
    }
}

/**
 * @type {(gl: WebGLRenderingContext) => {program: WebGLProgram, u_proj: WebGLUniformLocation, u_view: WebGLUniformLocation}}
 */
function initWebGLProgram(gl) {
    const program = gl.createProgram();

    const vertexShader = gl.createShader(gl.VERTEX_SHADER)
    gl.shaderSource(vertexShader, VERTEX_SHADER_TEXT);
    gl.compileShader(vertexShader); // |Fixme: Check compilation errors.

    const fragmentShader = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fragmentShader, FRAGMENT_SHADER_TEXT);
    gl.compileShader(fragmentShader); // |Fixme: Check compilation errors.

    gl.attachShader(program, vertexShader);
    gl.attachShader(program, fragmentShader);
    gl.linkProgram(program); // |Todo: Clean up shaders?
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        console.error("Shader program did not link successfully.");
        console.error("Error log: ", gl.getProgramInfoLog(program));
        return; // |Todo: Clean up GL program?
    }

    const buffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);

    const v_position = gl.getAttribLocation(program, "v_position");
    const v_colour   = gl.getAttribLocation(program, "v_colour");

    gl.enableVertexAttribArray(v_position);
    gl.enableVertexAttribArray(v_colour);

    gl.vertexAttribPointer(v_position, 2, gl.FLOAT, false, 5*4,   0);
    gl.vertexAttribPointer(v_colour,   3, gl.FLOAT, false, 5*4, 2*4); //|Cleanup: Avoid hard-coding these numbers.

    const u_proj = gl.getUniformLocation(program, "u_proj"); // Transforms pixel space to UV space. Only changes when the screen dimensions change.
    const u_view = gl.getUniformLocation(program, "u_view"); // Applies current pan/zoom. User-controlled.

    return {program, u_proj, u_view};
}

//
// User input stuff.
//

const input = {
    /**
     * pointers[0] will be the mouse, or the first finger to touch the screen. pointers[1] will be the second finger.
     * .down is true whenever the pointer is down. .pressed is true only on the first frame it's down.
     * .x and .y are in screen coordinates.
     *
     * @typedef {{id: number, x: number, y: number, down: boolean, pressed: boolean}} Pointer
     * @type {[Pointer, Pointer]}
     */
    pointers: [
        {id: 0, x: 0, y: 0, down: false, pressed: false},
        {id: 0, x: 0, y: 0, down: false, pressed: false},
    ],
    //|Todo: The pointers array currently always has two members. Maybe we would prefer it if it had a variable number: zero, one or two, depending on how many are currently relevant. Then we wouldn't need to do potentially pointless calculations in getPointerFlags() on pointers that aren't around any more. This optimisation is slightly harder than it sounds because each member of this array is associated with a map.pointerLock in the same index. There is also the fact that there may well be a greater performance penalty to creating/deleting these objects on the fly rather than just updating the ones we have.

    /** @type {number} */
    scroll: 0, // The deltaY of wheel events.

    /** @type {{[key: string]: boolean}} */
    pressed: {}, // E.g. {a: true, b: true}. |Cleanup: Rename keysPressed?
};

function initInput() {
    window.addEventListener("pointerdown", event => {
        for (let i = 0; i < 2; i++) {
            const ptr = input.pointers[i];

            if (ptr.down)  continue;

            ptr.id      = event.pointerId;
            ptr.x       = event.clientX;
            ptr.y       = event.clientY;
            ptr.down    = true;
            ptr.pressed = true;

            break;
        }
    });

    window.addEventListener("pointerup", event => {
        for (let i = 0; i < 2; i++) {
            const ptr = input.pointers[i];

            if (ptr.id !== event.pointerId)  continue;

            ptr.down = false;
            ptr.x    = event.clientX; // |Cleanup: Necessary?
            ptr.y    = event.clientY; // |Cleanup: Necessary?

            break;
        }
    });

    window.addEventListener("pointermove", event => {
        const [ptr0, ptr1] = input.pointers;

        if (!ptr0.down && !ptr1.down) {
            ptr0.x = event.clientX;
            ptr0.y = event.clientY;
        } else {
            for (let i = 0; i < 2; i++) {
                const ptr = input.pointers[i];

                if (ptr.id !== event.pointerId)  continue;

                ptr.x = event.clientX;
                ptr.y = event.clientY;

                break;
            }
        }
    });

    window.addEventListener("wheel", event => {
        input.scroll = event.deltaY;
    }, {passive: true});

    window.addEventListener("keydown", event => {
        input.pressed[event.key] = true;
    });
}

/** @enum {number} */
const Layer = {
    MAP:   1,
    PANEL: 2,
};

/**
 * Any time we draw a rectangle that catches mouse events so that events on anything drawn below should be ignored, we add a rectangle of occlusion.
 * The function getPointerFlags() looks at this occlusion array and returns pointer events that are not occluded.
 * Actually there are two occlusion arrays. getPointerFlags looks at the one built during the previous frame, occlusions[0].
 *
 * @typedef {{layer: number, rect: Rect}} Occlusion
 * @type [Occlusion[], Occlusion[]]
 */
const occlusions = [[], []];

/** @type {(layer: number, rect: Rect) => void} */
function addOcclusion(layer, rect) {
    occlusions[1].push({layer, rect});
}

// To be called once per frame.
function resetInput() {
    occlusions[0] = occlusions[1];
    occlusions[1] = [];

    input.pointers[0].pressed = false;
    input.pointers[1].pressed = false;
    input.scroll = 0;
    input.pressed = {};
}

/**
 * @typedef {{pressed?: true, hover?: true}} PointerFlags
 *
 * @type {(rect: Rect, layer: number) => [PointerFlags, PointerFlags]}
 */
function getPointerFlags(rect, layer) {
    /** @type [PointerFlags, PointerFlags] */
    const result = [{}, {}];

    //|Todo: See the Todo in the declaration of the global `input` variable. Note as well that there's really no such thing as input.pointers[1].hover, which we try to create in this function---because the second pointer is always a finger, which can't hover. It's either down or nowhere to speak of. That's why it'd be better for both input.pointers and the array returned by this function to be variable-length.
    for (let i = 0; i < 2; i++) {
        if (!pointInRect(input.pointers[i], rect))  continue;

        let occluded = false;

        for (let j = occlusions[0].length-1; j >= 0; j--) {
            if (occlusions[0][j].layer == layer)  break;

            if (pointInRect(input.pointers[0], occlusions[0][j].rect)) {
                occluded = true;
                break;
            }
        }

        if (!occluded) {
            if (input.pointers[i].pressed)     result[i].pressed = true;
            else if (!input.pointers[i].down)  result[i].hover   = true;
        }
    }

    return result;
}

document.addEventListener("DOMContentLoaded", async () => {
    loadFonts();
    initInput();

    /** @type WebGLRenderingContext */
    const gl = $("canvas#map").getContext("webgl");

    /** @type CanvasRenderingContext2D */
    const ui = $("canvas#gui").getContext("2d");

    const {program, u_proj, u_view} = initWebGLProgram(gl);


    let vertices = new Float32Array(0);
    let fetchVertices  = false;
    let updateVertices = false;

    /**
     * @typedef {{text: string, pos: [number, number]}} Label
     * @type Label[]
     */
    let labels; { //|Speed: Make this fetch async.
        const response = await fetch("../bin/labels.json");
        const json = await response.json();

        labels = json.labels;
    }

    /**
     * @typedef {{code: string, name: string, colour: string}} Party
     * @type {{[key: string]: Party}}
     */
     let parties;
     /** @type string[] */
     let partyCodes;
     { //|Speed: Make this fetch async.
        const response = await fetch("/parties");
        const json     = await response.json();
        parties = json.parties;

        partyCodes = [];
        for (const id of Object.keys(parties)) {
            const code = parties[id].code;
            if (code && partyCodes.indexOf(code) < 0)  partyCodes.push(code);
        }
     }


    const map = {
        //
        // Map constants:
        //
        minScale:  0.0001,
        maxScale:  0.5,
        maxScroll: 5000, // How far you have to scroll (in "pixels") to go from the minimum to maximum scale.

        //
        // Map state variables:
        //

        width:  document.body.clientWidth,
        height: document.body.clientHeight,

        /** @type {Transform} */
        currentTransform: {
            scale:      1,
            rotate:     0, // The angle, in radians, of a counter-clockwise rotation.
            translateX: 0,
            translateY: 0,
        },

        // When the user presses their mouse down on the map, we lock the mouse position to its
        // current location on the map. On a touchscreen, we do this with up to two fingers.
        /** @typedef {{locked: boolean, x: number, y: number}} PointerLock */
        /** @type {[PointerLock, PointerLock]} */
        pointerLocks: [  // X and Y are in map coordinates.
            {locked: false, x: 0, y: 0},
            {locked: false, x: 0, y: 0},
        ],

        /** @typedef {{
         *      startTime: DOMHighResTimeStamp,
         *      endTime:   DOMHighResTimeStamp,
         *      start:     Transform,
         *      end:       Transform,
         *      scroll?:   true
         *  }} Animation */
        /** @type Array<Animation> */
        animations: [],

        // This value represents how far the user has scrolled in "pixels", if you imagine the map
        // is a normal web page where, as you scroll, the map goes from minimum to maximum zoom.
        // This value is only valid if there is currently a scroll animation happening. Otherwise it
        // must be recalculated from map.currentTransform.scale.
        /** @type {number} */
        scrollOffset: 0,
    };

    // The UI panel can be in desktop mode or mobile mode. In desktop mode, we calculate its position as a
    // function of the screen size each frame. In mobile mode (which for now we assume the phone is held in
    // portrait), it sits at the bottom of the screen and the user can slide it up and down. That's the
    // as-yet-unimplemented goal, anyway.
    let mobileMode = false;

    //|Temporary: this should go in the map state and also be an enum or something, not a boolean.
    let isFirstPreferences = false;

    /**
     * @type Rect
     */
    const panelRect = {
        x:      map.width/2, //|Cleanup: These are just nonsense values. We set them in the main loop.
        y:      map.height/2,
        width:  0.4*map.width,
        height: 400
    };
    let panelIsBeingDragged = false;
    let panelDragStartY     = 0; // If the panel is being dragged, this is the Y value of the pointer when the drag was last done.

    // Toggle developer visualisations. |Debug
    let debugTransform = false;
    let debugLabels    = false;
    let debugFPS       = false;

    // The number of milliseconds since page load. Calculated once per frame.
    // For animations, not performance testing.
    let currentTime = document.timeline.currentTime;

    // Stuff for FPS calculations: |Debug
    const maxPerfSamples   = 32;
    const timeDeltaSamples = new Float32Array(maxPerfSamples);
    const timeUsedSamples  = new Float32Array(maxPerfSamples);
    let numPerfSamples = 0;
    let fpsText        = '';
    let fpsTextUpdated = currentTime;

    //
    // The main loop (runs once per frame):
    //
    ;(function step(time) {
        //
        // Update state.
        //
        const timeDelta = time - currentTime;
        currentTime = time;

        let frameStartTime; // |Debug
        if (debugFPS)  frameStartTime = performance.now();

        map.width  = document.body.clientWidth;
        map.height = document.body.clientHeight;

        // Handle mouse/touch events on the map.
        {
            const mapRect  = {x: 0, y: 0, width: map.width, height: map.height};//|Todo: Store this on the map object. Use it to replace map.width and .height.
            const ptrFlags = getPointerFlags(mapRect, Layer.MAP);

            const ptr  = input.pointers;
            const lock = map.pointerLocks;
            const ct   = map.currentTransform;

            for (let i = 0; i < 2; i++) {
                if (ptrFlags[i].pressed) {
                    if (!lock[i].locked) {
                        // The user has just pressed the mouse or touched the screen.
                        lock[i].locked = true;

                        const pointerMapCoords = inverseXform(ct, ptr[i]);

                        lock[i].x = pointerMapCoords.x;
                        lock[i].y = pointerMapCoords.y;

                        map.animations.length = 0; // Cancel any current animations.
                    }
                } else if (!ptr[i].down) {
                    lock[i].locked = false;
                }
            }

            if (lock[0].locked && lock[1].locked) {
                // Two fingers are currently moving the map.

                // Find scale:
                const mapDistanceX = lock[1].x - lock[0].x;
                const mapDistanceY = lock[1].y - lock[0].y;
                const mapDistance  = Math.hypot(mapDistanceX, mapDistanceY);

                const screenDistanceX = ptr[1].x - ptr[0].x;
                const screenDistanceY = ptr[1].y - ptr[0].y;
                const screenDistance  = Math.hypot(screenDistanceX, screenDistanceY);

                ct.scale = screenDistance/mapDistance;

                // Find rotation:
                // |Bug: The below calculations break when d == 0 and maybe other times?
                let mapAngle; {
                    const dx = mapDistanceX;
                    const dy = mapDistanceY;
                    const d  = mapDistance;

                    if      (dy >= 0 && dx > 0)  mapAngle = Math.asin(dy/d);                 // Q1
                    else if (dy >= 0 && dx < 0)  mapAngle = Math.acos(dy/d) + Math.PI/2;     // Q2
                    else if (dy <= 0 && dx < 0)  mapAngle = Math.asin(-dy/d) + Math.PI;      // Q3
                    else                         mapAngle = Math.acos(-dy/d) + 3*Math.PI/2;  // Q4
                }
                let screenAngle; {
                    const dx = screenDistanceX;
                    const dy = screenDistanceY;
                    const d  = screenDistance;

                    if      (dy >= 0 && dx > 0)  screenAngle = Math.asin(dy/d);                 // Q1
                    else if (dy >= 0 && dx < 0)  screenAngle = Math.acos(dy/d) + Math.PI/2;     // Q2
                    else if (dy <= 0 && dx < 0)  screenAngle = Math.asin(-dy/d) + Math.PI;      // Q3
                    else                         screenAngle = Math.acos(-dy/d) + 3*Math.PI/2;  // Q4
                }

                ct.rotate = mapAngle - screenAngle;

                // Find translation:
                const lockScreenCoords = xform(ct, lock[0]);

                ct.translateX += ptr[0].x - lockScreenCoords.x;
                ct.translateY += ptr[0].y - lockScreenCoords.y;
            } else {
                for (let i = 0; i < 2; i++) {
                    if (lock[i].locked && !lock[1-i].locked) {
                        const lockScreenCoords = xform(ct, lock[i]);

                        ct.translateX += ptr[i].x - lockScreenCoords.x;
                        ct.translateY += ptr[i].y - lockScreenCoords.y;

                        break;
                    }
                }
            }
        }

        // Handle scroll.
        if (input.scroll) {
            const {minScale, maxScale, maxScroll} = map;
            const ct = map.currentTransform;

            const exp0 = Math.log2(minScale); // |Cleanup. We'd rather store the exponential value. Transform.scale should also be the exponential.
            const exp1 = Math.log2(maxScale);

            if (!map.animations.length || !map.animations[0].scroll) {
                // There is not currently a scroll animation happening, so we can't trust the
                // map.scrollOffset variable. Calculate it again based on the current scale.
                const exp = Math.log2(ct.scale); // |Fixme: What if this is outside our expected range?
                const t   = (exp - exp0)/(exp1 - exp0);

                map.scrollOffset = maxScroll*t;
            }

            map.scrollOffset -= input.scroll;
            if (map.scrollOffset < 0)               map.scrollOffset = 0;
            else if (map.scrollOffset > maxScroll)  map.scrollOffset = maxScroll;

            const t   = map.scrollOffset/maxScroll;
            const exp = lerp(exp0, exp1, t);

            const newTransform = clone(ct);
            newTransform.scale = Math.pow(2, exp);

            const mouse  = input.pointers[0];
            const origin = inverseXform(ct, mouse);

            // |Naming: These variables. Call them something like "error", "correction", "offset"?
            const originScreenCoords = xform(newTransform, origin);
            newTransform.translateX += mouse.x - originScreenCoords.x;
            newTransform.translateY += mouse.y - originScreenCoords.y;

            const duration = 100;

            map.animations.length = 0;
            map.animations.push({
                startTime: currentTime,
                endTime:   currentTime + duration,
                start:     clone(ct),
                end:       newTransform,
                scroll:    true, // A special flag just for scroll-zoom animations, so we know we can trust map.scrollOffset.
            });
        }

        // Handle keyboard presses.
        {
            // When the user presses certain numbers, animate the map to show different locations. |Temporary
            const aust = [{x:-1863361, y: 1168642}, {x: 2087981, y: 4840595}];
            const melb = [{x: 1140377, y: 4187714}, {x: 1149001, y: 4196377}];
            const syd  = [{x: 1757198, y: 3827047}, {x: 1763103, y: 3834946}];

            const boxes = {'0': aust, '1': melb, '2': syd};

            for (const key of Object.keys(boxes)) {
                if (input.pressed[key]) {
                    const targetBox = boxes[key];

                    const corners  = getMapCorners(map.width, map.height, map.currentTransform);
                    const envelope = getEnvelope(corners);

                    const combined = combineBoxes(targetBox, envelope);

                    // It's a simple transition if one of the boxes contains the other.
                    const simple = (combined === targetBox) || (combined === envelope);

                    if (simple) {
                        const duration = 1000;

                        /** @type Box */
                        const screen = [{x: 0, y: 0}, {x: map.width, y: map.height}];

                        const newTransform = fitBox(targetBox, screen);

                        map.animations.length = 0;
                        map.animations.push({
                            startTime: currentTime,
                            endTime:   currentTime + duration,
                            start:     clone(map.currentTransform),
                            end:       newTransform,
                        });
                    } else {
                        const durations = [750, 750];

                        /** @type Box */
                        const screen = [{x: 0, y: 0}, {x: map.width, y: map.height}];

                        // |Todo: Expand the combined box by 10%.
                        const transform1 = fitBox(combined, screen);
                        const transform2 = fitBox(targetBox, screen);

                        map.animations.length = 0;
                        map.animations.push({
                            startTime: currentTime,
                            endTime:   currentTime + durations[0],
                            start:     clone(map.currentTransform),
                            end:       transform1,
                        });
                        map.animations.push({
                            startTime: currentTime + durations[0],
                            endTime:   currentTime + durations[0] + durations[1],
                            start:     transform1,
                            end:       transform2,
                        });
                    }
                }
            }

            // Refetch vertices when the user presses 'r'. |Temporary
            if (input.pressed['r'])  fetchVertices = true;

            // Check whether developer visualisations have been toggled:
            if (input.pressed['t'])  debugTransform = !debugTransform;
            if (input.pressed['l'])  debugLabels    = !debugLabels;
            if (input.pressed['f'])  debugFPS       = !debugFPS;
        }

        // Fetch vertex data.
        if (fetchVertices) {
            ;(async function() {
                fetchVertices = false;

                const corners  = getMapCorners(map.width, map.height, map.currentTransform);
                const envelope = getEnvelope(corners);

                let url = '../bin/vertices';
                url += '?';
                url += '&x0=' + envelope[0].x;
                url += '&y0=' + envelope[0].y;
                url += '&x1=' + envelope[1].x;
                url += '&y1=' + envelope[1].y;

                // UPP: Map units per pixel. Increases as you zoom out.
                const upp = 1/map.currentTransform.scale;
                url += '&upp=' + upp;

                const response = await fetch(url);
                const data = await response.arrayBuffer();

                vertices = new Float32Array(data);

                updateVertices = true;
            })();
        }

        // Apply animations.
        {
            while (map.animations.length) {
                const {startTime, endTime, start, end} = map.animations[0];
                const ct = map.currentTransform;

                if (currentTime < startTime)  break;

                if (currentTime < endTime) {
                    const keys = ["rotate", "translateX", "translateY"];

                    const t = (currentTime - startTime)/(endTime - startTime);

                    if (end.scale === start.scale) {
                        keys.push("scale");
                        for (const key of keys)  ct[key] = lerp(start[key], end[key], t);
                    } else {
                        // |Speed: Store exp0 and exp1 on the animation object.
                        const exp0 = Math.log2(start.scale);
                        const exp1 = Math.log2(end.scale);
                        const exp  = lerp(exp0, exp1, t);

                        ct.scale = Math.pow(2, exp);

                        const t2 = (ct.scale - start.scale)/(end.scale - start.scale);

                        for (const key of keys)  ct[key] = lerp(start[key], end[key], t2);
                    }

                    break; // The first animation is ongoing, so we don't need to check the next one.
                }

                // The first animation has completed.
                const keys = ["scale", "rotate", "translateX", "translateY"];
                for (const key of keys)  ct[key] = end[key];

                map.animations.shift(); // Remove the first animation (and check the next one).
            }
        }

        const proj = new Float32Array([1,0,0, 0,1,0, 0,0,1]);
        {
            // Transform from pixel space to UV space. Flip the y-axis for top-left origin.
            proj[0] =  2/map.width;       // X scale.
            proj[4] = -2/map.height;      // Y scale.
            proj[6] = -1;                 // X translate.
            proj[7] =  1;                 // Y translate.
        }

        const view = new Float32Array([1,0,0, 0,1,0, 0,0,1]);
        {
            const {scale, rotate, translateX, translateY} = map.currentTransform;

            const cos = Math.cos(rotate);
            const sin = Math.sin(rotate);

            view[0] = scale*cos;
            view[1] = -scale*sin;
            view[3] = scale*sin;
            view[4] = scale*cos;
            view[6] = translateX;
            view[7] = translateY;
        }

        //
        // Draw the page.
        //

        const dpr = window.devicePixelRatio || 1;

        $$("canvas").forEach(canvas => {
            canvas.width  = Math.floor(dpr*map.width);
            canvas.height = Math.floor(dpr*map.height);

            canvas.style.width  = map.width + "px";
            canvas.style.height = map.height + "px";
        });

        // WebGL canvas:
        {
            gl.viewport(0, 0, dpr*map.width, dpr*map.height);

            gl.clearColor(0.1, 0.1, 0.1, 1.0); // Background colour (same as water): off-black.
            gl.clear(gl.COLOR_BUFFER_BIT);

            gl.useProgram(program);

            gl.uniformMatrix3fv(u_proj, false, proj);
            gl.uniformMatrix3fv(u_view, false, view);

            if (updateVertices) {
                gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);
                updateVertices = false;
            }

            gl.drawArrays(gl.TRIANGLES, 0, vertices.length/5); // 5 is the number of floats per vertex attribute.

            const error = gl.getError();
            if (error)  console.error(`WebGL error. Code: ${error}`);
        }

        // 2D canvas:
        {
            ui.scale(dpr, dpr);
            ui.clearRect(0, 0, map.width, map.height);

            //
            // Draw the labels from the JSON file, as stably as possible!
            //
            {
                const height = 16; // Text height.
                ui.font = `${height}px map-electorate`;
                ui.textBaseline = "top";

                //
                // We want to break the screen up into a grid of squares. Then, when we draw labels, we
                // can mark the squares we've drawn on, and in this way prevent labels from overlapping.
                //   The complexity comes from the fact that we want our grid to be aligned with the
                // map's transform. That's how we get labels that are mostly stable across transforms.
                //   Our current approach involves expanding everything into rectangles aligned with the
                // XY-axes of the map's coordinates. We do this with the screen's bounds initially,
                // and again with the labels' text boxes.
                //   When the map is rotated, the screen's bounding box becomes a diamond in map-space.
                // So when we draw a rectangle around the diamond and establish our grid there,
                // some of the grid's squares end up being off-screen, where they're not so useful.
                //   Similarly, when the map is rotated, labels become diagonal relative to the
                // map's axes, so they end up taking all the grid-squares required by the rectangles
                // enclosing their diagonals.
                //   When the map has not been rotated, our approach is fine, because the screen and
                // text boxes are their smallest enclosing orthogonal rectangles already.
                //   (When we start rotating the labels themselves, that won't be true anymore.)
                //   So, for now, the below algorithm performs worse when the map is rotated. |Temporary.
                //
                const resolution = 256;
                const usedSpace  = new Int8Array(resolution);

                let gridSize, numGridCols, numGridRows, gridRect; {
                    // Find the map-axis-aligned rectangle enclosing the screen's bounding box.
                    const corners    = getMapCorners(map.width, map.height, map.currentTransform);
                    const [min, max] = getEnvelope(corners);

                    const focusWidth  = max.x - min.x;
                    const focusHeight = max.y - min.y;

                    const ratio = focusWidth/focusHeight;

                    const numRows = Math.sqrt(resolution/ratio);  // Not the real number of rows in our grid, but an intermediary.
                    const rowHeight = focusHeight/numRows;

                    // Round up to the nearest power of two.
                    let mask = 1;
                    while (mask < rowHeight)  mask <<= 1;
                    gridSize = mask; // gridSize is in map units.

                    numGridRows = Math.floor(numRows);            // The real number of rows in our grid.
                    numGridCols = Math.floor(resolution/numRows); // The real number of columns in our grid.

                    gridRect = {
                        x:      min.x - (min.x % gridSize),
                        y:      min.y - (min.y % gridSize),
                        width:  numGridCols*gridSize,
                        height: numGridRows*gridSize,
                    };
                }

                // Draw the labels.
                for (const label of labels) {
                    const {width} = ui.measureText(label.text);
                    const screenPos = xform(map.currentTransform, {x: label.pos[0], y: label.pos[1]});
                    const textX = screenPos.x - width/2;
                    const textY = screenPos.y - height/2;
                    const textX1 = textX + width; // textX1 and textY1 are the bottom-right corner of the text box.
                    const textY1 = textY + height;

                    // Find the map-axis-aligned rectangle enclosing the text box:
                    /** @type Box4 */
                    const box = [
                        {x: textX,  y: textY},
                        {x: textX,  y: textY1},
                        {x: textX1, y: textY1},
                        {x: textX1, y: textY}
                    ];
                    for (let i = 0; i < 4; i++)  box[i] = inverseXform(map.currentTransform, box[i]);

                    const [min, max] = getEnvelope(box);

                    // Don't draw labels that are outside our grid:
                    if (min.y > gridRect.y + gridRect.height)  continue;
                    if (max.y < gridRect.y)                    continue;
                    if (min.x > gridRect.x + gridRect.width)   continue;
                    if (max.x < gridRect.x)                    continue;

                    // Check whether any of the grid squares we want have been taken.

                    let used = false;

                    const col0 = Math.floor((min.x - gridRect.x)/gridSize);
                    const row0 = Math.floor((min.y - gridRect.y)/gridSize);
                    const col1 = Math.ceil((max.x - gridRect.x)/gridSize);
                    const row1 = Math.ceil((max.y - gridRect.y)/gridSize);

                    {
                        let row = row0;
                        let col = col0;
                        while (row < row1) {
                            const index = row*numGridCols + col;
                            if (usedSpace[index]) {
                                // We can't use this space.
                                used = true;
                                break;
                            }

                            if (col < col1) {
                                col += 1;
                            } else {
                                row += 1;
                                col = col0;
                            }
                        }
                    }

                    // We've failed to find room for this label, but keep trying subsequent labels.
                    if (used)  continue;

                    // We are now going to use the space. Mark the squares as used.
                    for (let row = row0; row < row1; row++) {
                        for (let col = col0; col < col1; col++) {
                            const index = row*numGridCols + col;
                            usedSpace[index] = 1;
                        }
                    }

                    ui.strokeStyle = 'white';
                    ui.lineWidth = 3;
                    ui.strokeText(label.text, textX, textY);
                    ui.fillStyle = 'black';
                    ui.fillText(label.text, textX, textY);
                }

                if (debugLabels) { // Visualise the usedSpace grid. |Debug
                    ui.lineWidth = 1;
                    ui.strokeStyle = 'rgba(255,255,255,0.5)';

                    const ct = map.currentTransform;
                    const gr = gridRect;

                    for (let i = 0; i < numGridCols+1; i++) {
                        const {x: x1, y: y1} = xform(ct, {x: gr.x + gridSize*i, y: gr.y}); // |Cleanup. Make this point a variable and add gridSize each pass?
                        const {x: x2, y: y2} = xform(ct, {x: gr.x + gridSize*i, y: gr.y + gr.height});
                        ui.moveTo(x1, y1);
                        ui.lineTo(x2, y2);
                    }
                    for (let i = 0; i < numGridRows+1; i++) {
                        const {x: x1, y: y1} = xform(ct, {x: gr.x,            y: gr.y + gridSize*i});
                        const {x: x2, y: y2} = xform(ct, {x: gr.x + gr.width, y: gr.y + gridSize*i});
                        ui.moveTo(x1, y1);
                        ui.lineTo(x2, y2);
                    }
                    ui.stroke();

                    ui.fillStyle = 'rgba(255,255,255,0.35)';
                    ui.beginPath();
                    for (let row = 0; row < numGridRows; row++) {
                        for (let col = 0; col < numGridCols; col++) {
                            const index = numGridCols*row + col;
                            if (usedSpace[index]) {
                                // |Speed: This is very slow when the screen has a non-zero rotation!
                                const [p0, p1, p2, p3] = [
                                    {x: gr.x + gridSize*col,            y: gr.y + gridSize*row},
                                    {x: gr.x + gridSize*col,            y: gr.y + gridSize*row + gridSize},
                                    {x: gr.x + gridSize*col + gridSize, y: gr.y + gridSize*row + gridSize},
                                    {x: gr.x + gridSize*col + gridSize, y: gr.y + gridSize*row},
                                ].map(
                                    point => xform(ct, point)
                                );
                                ui.moveTo(p0.x, p0.y);
                                ui.lineTo(p1.x, p1.y);
                                ui.lineTo(p2.x, p2.y);
                                ui.lineTo(p3.x, p3.y);
                            }
                        }
                    }
                    ui.closePath();
                    ui.fill();
                }
            }

            //
            // Draw the panel.
            //
            {
                if (document.body.clientWidth < 450) {
                    if (!mobileMode) {
                        // The user has just switched to mobile mode.
                        mobileMode = true;

                        panelRect.x      = 0;
                        panelRect.y      = 0.75*document.body.clientHeight;
                        panelRect.height = document.body.clientHeight - panelRect.y;
                    } else {
                        // Otherwise, if the user was already in mobile mode, the user controls the panel's dimensions.
                        panelRect.width = document.body.clientWidth;

                        if (panelIsBeingDragged) {
                            if (!input.pointers[0].down) {
                                panelIsBeingDragged = false;
                            } else {
                                const diff = input.pointers[0].y - panelDragStartY;
                                let dragAllowed = true; {
                                    const alwaysShow = 20; // Never allow the panel to be pushed further than would show its top 20 pixels.
                                    if (panelRect.y + diff > document.body.clientHeight - alwaysShow)             dragAllowed = false;
                                    else if (panelRect.y + panelRect.height + diff < document.body.clientHeight)  dragAllowed = false;
                                }
                                if (dragAllowed) {
                                    panelRect.y    += diff;
                                    panelDragStartY = input.pointers[0].y;
                                }
                            }
                        } else if (mobileMode) {
                            //|Todo: cutTop
                            const dragRect  = clone(panelRect);
                            dragRect.height = 50;

                            const flags = getPointerFlags(dragRect, Layer.PANEL);
                            if (flags[0].pressed) {
                                panelIsBeingDragged = true;
                                panelDragStartY     = input.pointers[0].y;
                            }
                        }

                        // Make sure we aren't leaving a gap at the bottom of the page.
                        const gap = document.body.clientHeight - (panelRect.y + panelRect.height);
                        if (gap > 0)  panelRect.y += gap;
                    }
                } else {
                    mobileMode = false;

                    const margin    = 10;
                    panelRect.x     = margin;
                    panelRect.y     = margin;
                    panelRect.width = 0.34*document.body.clientWidth;
                    // The height gets set at the end of this scope, for the next frame.
                }

                addOcclusion(Layer.PANEL, panelRect);

                ui.fillStyle = 'rgba(255, 255, 255, 0.95)';
                ui.fillRect(panelRect.x, panelRect.y, panelRect.width, panelRect.height);

                const panelPadding = 10;

                const panelX     = panelRect.x + panelPadding;
                const panelWidth = panelRect.width - 2*panelPadding;

                let panelY = panelRect.y + panelPadding;

                // Draw the election title.
                {
                    let height = 40;
                    let text = "2022 Federal Election";
                    ui.fillStyle = 'black';

                    ui.font = height + 'px title';
                    let textWidth = ui.measureText(text).width;

                    if (textWidth < panelWidth) {
                        ui.fillText(text, panelX + panelWidth/2 - textWidth/2, panelY);
                        panelY += height;
                    } else {
                        text = '2022';
                        textWidth = ui.measureText(text).width;
                        ui.fillText(text, panelX + panelWidth/2 - textWidth/2, panelY);
                        panelY += height;

                        text = 'Federal Election';
                        height = 30;
                        ui.font = height + 'px title';
                        textWidth = ui.measureText(text).width;

                        if (textWidth < panelWidth) {
                            ui.fillText(text, panelX + panelWidth/2 - textWidth/2, panelY);
                            panelY += height;
                        } else {
                            height = 25;
                            ui.font = height + 'px title';

                            text = 'Federal';
                            textWidth = ui.measureText(text).width;
                            ui.fillText(text, panelX + panelWidth/2 - textWidth/2, panelY);
                            panelY += height;

                            text = 'Election';
                            textWidth = ui.measureText(text).width;
                            ui.fillText(text, panelX + panelWidth/2 - textWidth/2, panelY);
                            panelY += height;
                        }
                    }
                }

                panelY += panelPadding;

                // Draw the two-candidate preferred/first preferences toggle.
                {
                    // Styles:
                    const toggleHeight      = 30;
                    const backgroundColours = ['#dddddd', '#ffffff']; // [unselected, selected]
                    const textColours       = ['#777777', '#000000'];
                    const fonts             = ['button-inactive', 'button-active']; //|Cleanup: Selected or active?
                    const borderColour      = '#000000';
                    const borderWidth       = 1;
                    const leftLabel         = "2CP"; //|Todo: Spell out the selected option in full.
                    const rightLabel        = "FP";  //|

                    // Computed styles:
                    const textMargin = Math.floor(toggleHeight/10);
                    const textHeight = toggleHeight - 2*textMargin;
                    const toggleRect = {x: panelX, y: panelY, width: panelWidth, height: toggleHeight};
                    const [leftRect, rightRect] = cutLeft(toggleRect, toggleRect.width/2);

                    // Change isFirstPreferences if the mouse is down over the unselected button.
                    {
                        const unselectedRect = (isFirstPreferences) ? leftRect : rightRect;

                        const pointerFlags = getPointerFlags(unselectedRect, Layer.PANEL);

                        if (pointerFlags[0].pressed)  isFirstPreferences = !isFirstPreferences;
                    }

                    // Draw the buttons.
                    for (let i = 0; i < 2; i++) {
                        const rect  = (i == 0) ? leftRect  : rightRect;
                        const label = (i == 0) ? leftLabel : rightLabel;

                        const selected = +(!i == !isFirstPreferences);

                        ui.fillStyle = backgroundColours[selected];
                        ui.fillRect(rect.x, rect.y, rect.width, rect.height);

                        ui.font      = textHeight + 'px ' + fonts[selected];
                        ui.fillStyle = textColours[selected];
                        const textWidth = ui.measureText(label).width;
                        ui.fillText(label, rect.x + rect.width/2 - textWidth/2, rect.y + textMargin);
                    }

                    // Draw a border.
                    ui.strokeStyle = borderColour;
                    ui.lineWidth   = borderWidth;
                    ui.strokeRect(toggleRect.x, toggleRect.y, toggleRect.width, toggleRect.height);

                    panelY += toggleRect.height;

                    if (isFirstPreferences && partyCodes) {
                        const height = 10;
                        ui.font = height + 'px party-label';

                        for (const label of partyCodes) {
                            ui.fillText(label, panelX, panelY);
                            panelY += height;
                        }
                    }
                }

                panelY += panelPadding;

                // For the next frame, set the panel's height to the used height.
                panelRect.height = panelY - panelRect.y;
            }

            // Draw the map's current transform in the bottom-right corner of the canvas. |Debug
            if (debugTransform) {
                const height = 16; // Text height.
                ui.font = height + 'px sans-serif';
                ui.textBaseline = "top";

                let y = map.height - height;

                for (const target of ["translateY", "translateX", "rotate", "scale"]) {
                    const label = target + ': ' + map.currentTransform[target];
                    const width = 200;
                    const x     = map.width - width;

                    ui.fillStyle = 'rgba(255,255,255,0.9)';
                    ui.fillRect(x, y, width, height);

                    ui.fillStyle = 'black';
                    ui.fillText(label, x, y);

                    y -= height;
                }
            }

            // Display FPS: |Debug
            if (debugFPS) {
                // debugFPS will be true if the user pressed 'f' this frame, but we can only start taking
                // performance samples on the next frame, because we need the frameStartTime. |Debug
                const firstFrame = (frameStartTime === undefined);
                if (firstFrame) {
                    numPerfSamples = 0;
                    fpsTextUpdated = currentTime;
                } else {
                    const index = numPerfSamples % maxPerfSamples;
                    timeDeltaSamples[index] = timeDelta;
                    timeUsedSamples[index]  = performance.now() - frameStartTime;
                    numPerfSamples += 1;

                    // Update the FPS text no more than this many times a second (to make it more readable).
                    const rate = 5;

                    if (numPerfSamples > 4 && fpsTextUpdated+(1000/rate) < currentTime) {
                        let meanTimeDelta, meanTimeUsed; {
                            const numSamples = Math.min(numPerfSamples, maxPerfSamples);
                            let sumTimeDelta = 0;
                            let sumTimeUsed  = 0;
                            for (let i = 0; i < numSamples; i++) {
                                sumTimeDelta += timeDeltaSamples[i];
                                sumTimeUsed  += timeUsedSamples[i];
                            }
                            meanTimeDelta = sumTimeDelta/numSamples;
                            meanTimeUsed  = sumTimeUsed/numSamples;
                        }

                        const fps = 1000/meanTimeDelta;

                        fpsText  = '';
                        fpsText += `Quota: ${meanTimeDelta.toFixed(1)}ms for ${fps.toFixed(0)}Hz. `;
                        fpsText += `Used: ${meanTimeUsed.toFixed(1)}ms.\n`;

                        fpsTextUpdated = currentTime;
                    }

                    const textHeight = 14;
                    ui.font          = `${textHeight}px monospace`;
                    ui.textBaseline  = "top";

                    let x = 5;
                    let y = 5;
                    for (const line of fpsText.split('\n')) {
                        ui.fillStyle = 'black';
                        ui.fillText(line, x+1, y+1);
                        ui.fillStyle = 'white';
                        ui.fillText(line, x, y);
                        y += textHeight;
                    }
                }
            }
        }

        resetInput();

        window.requestAnimationFrame(step);
    })();

    // When the page loads, fit Australia on the screen.
    {
        /** @type Box */
        const aust = [{x:-1863361, y: 1168642}, {x: 2087981, y: 4840595}];
        /** @type Box */
        const screen = [{x: 0, y: 0}, {x: map.width, y: map.height}];

        map.currentTransform = fitBox(aust, screen);

        fetchVertices = true;
    }

    // For debugging:
    Object.assign(window, {map, gl, ui});
});
