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


        @typedef {{
                firstName:      string,
                lastName:       string,
                partyName:      string,
                partyCode:      string,
                colour:         string,
                numVotes:       number,
                ballotPosition: number,
            }} VoteCount

        @typedef {{
                name:           string,
                centroid:       Vec2,
                box:            Box,
                votes?:         {tcp: [VoteCount, VoteCount], fp: VoteCount[]},
            }} District

        @typedef {{
                id:             number,
                date:           Date,
                districts?:     {[key: number]: District},
                seatsWon?:      {alp: number, lnp: number, etc: number},
            }} Election


        @typedef {{locked: boolean, x: number, y: number}} PointerLock

        @typedef {{
               startTime: number,
               endTime:   number,
               start:     Transform,
               end:       Transform,
               scroll?:   true
           }} MapAnimation

  A tileset is an image that you want to show on the map at a particular resolution. For example, the election boundaries for 2016,
  at a resolution of 32 metres to a pixel, is one tileset. The Tileset object is keyed by strings of the form "x,y".
  These are the x and y values at the top-left corner of each tile, with a comma in between.

        @typedef {{[key: string]: Float32Array}} Tileset

  A dynamic tileset is an object where each key is a resolution and the values are tilesets with the same image at different resolutions.

        @typedef {{[key: number]: Tileset}} DynamicTileset
 */

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

//
// Application state globals.
//

/**
 * The number of milliseconds since page load. It stays the same for a whole frame, so we can't use it for intra-frame performance testing.
 * @type number
 */
let currentTime = document.timeline.currentTime;
/** @type number */
let timeDelta;

/** @type number */
let dpr = window.devicePixelRatio || 1;

const input = {
    /**
     * pointers[0] will be the mouse, or the first finger to touch the screen. pointers[1] will be the second finger.
     * .down is true whenever the pointer is down.
     * .pressed is true only on the first frame it's down.
     * .released is true only on the first frame it's up.
     * .x and .y are in screen coordinates.
     * We set .active to true on most pointer events and then set it to false on pointerout. This ensures that e.g. fingers touching the screen are inactive once they leave, as opposed to a mouse, which is still considered to be hovering.
     *
     * @typedef {{id: number, x: number, y: number, active: boolean, down: boolean, pressed: boolean, released: boolean}} Pointer
     *
     * @type {[Pointer, Pointer]}
     */
    pointers: [
        {id: 0, x: 0, y: 0, active: false, down: false, pressed: false, released: false},
        {id: 0, x: 0, y: 0, active: false, down: false, pressed: false, released: false},
    ],

    /**
     * We keep track of the time and location of the last pointer press so that we can tell whether something was tapped.
     * The index of the events matches the index in the pointers array.
     *
     * @typedef {{time: number, x: number, y: number}} PointerPressEvent
     *
     * @type {[PointerPressEvent, PointerPressEvent]}
     */
    pointersLastPress: [
        {time: 0, x: 0, y: 0},
        {time: 0, x: 0, y: 0},
    ],

    /**
     * The deltaY of wheel events.
     * @type number
     */
    scroll: 0,

    /**
     * The keys are the codes of keydown events. E.g. {KeyA: true, Digit1: true}.
     * @type {{[key: string]: boolean}}
     */
    keysPressed: {},

    /**
     * We can make this into a keyFlags object later if we want ctrl/alt.
     * @type boolean
     */
    shift: false,
};

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
 *
 * @type [Occlusion[], Occlusion[]]
 */
const occlusions = [[], []];

// The UI panel can be in desktop mode or mobile mode. In desktop mode, we calculate its position as a
// function of the screen size each frame. In mobile mode (which for now we assume the phone is held in
// portrait), it sits at the bottom of the screen and the user can slide it up and down. That's the
// as-yet-unimplemented goal, anyway.
let mobileMode = false;

/**
 * @type Election[]
 */
const elections = [
    {id: 15508, date: new Date("2010-08-21")},
    {id: 17496, date: new Date("2013-09-07")},
    {id: 20499, date: new Date("2016-07-02")},
    {id: 24310, date: new Date("2019-05-18")},
    {id: 27966, date: new Date("2022-05-21")},
];
/** @type number */
let currentElectionIndex = elections.length-1;

/** @type number */
let currentDistrictID = -1; // There's no district selected if it's -1.

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

    width:  0,
    height: 0,

    /** @type {Transform} */
    currentTransform: {
        scale:      1,
        rotate:     0, // The angle, in radians, of a counter-clockwise rotation. Always in the range [-Math.PI, Math.PI).
        translateX: 0,
        translateY: 0,
    },

    /**
     * When the user presses their mouse down on the map, we lock the mouse position to its current location
     * on the map. On a touchscreen, we do this with up to two fingers.
     *
     * @type {[PointerLock, PointerLock]}
     */
    pointerLocks: [  // X and Y are in map coordinates.
        {locked: false, x: 0, y: 0},
        {locked: false, x: 0, y: 0},
    ],

    /** @type Array<MapAnimation> */
    animations: [],

    /**
     * This value represents how far the user has scrolled in "pixels", if you imagine the map is a normal
     * web page where, as you scroll, the map goes from minimum to maximum zoom. This value is only valid if
     * there is currently a scroll animation happening. Otherwise it must be recalculated from map.currentTransform.scale.
     *
     * @type {number}
     */
    scrollOffset: 0,
};

/** @type {{[key: string]: Transform }} */
const savedTransforms = {};


//
// WebGL-related globals.
//

/** @type WebGLRenderingContext */
let gl;
/** @type WebGLProgram */
let program;
/** @type WebGLUniformLocation */
let u_proj;
/** @type WebGLUniformLocation */
let u_view;
/** @type Float32Array */
let vertices = new Float32Array(0);
/** @type boolean */
let updateVertices = false;

/**
 * For now, we only want to have five dynamic tilesets: one for each election. So the tileStore object is keyed by the election ID.
 *
 * @type {{[key: number]: DynamicTileset}}
 */
const tileStore = {};
/**
 * currentTileset is a subset of an tileset object within the tileStore, with only those keys needed to cover the screen currently.
 *
 * @type Tileset
 */
let currentTileset = {};
/** @type boolean */
let isVerticesRequestPending = false;


//
// UI-related globals.
//

/** @type CanvasRenderingContext2D */
let ui;

//|Cleanup: Merge these into a panel object.
/** @type Rect */
const panelRect = {
    x:      map.width/2, //|Cleanup: These are just nonsense values. We set them in the main loop.
    y:      map.height/2,
    width:  0.4*map.width,
    height: 400
};
/** @type boolean */
let panelIsBeingDragged = false;
/** @type number */
let panelDragStartY = 0; // If the panel is being dragged, this is the Y value of the pointer when the drag was last done.

//|Cleanup: Put these into a button state object.
/** @type number */
let prevElectionButtonLastPressed;
/** @type number */
let nextElectionButtonLastPressed;

//
// Toggle developer visualisations.
//

/** @type boolean */
let debugTransform = false;
/** @type boolean */
let debugLabels = false;
/** @type boolean */
let debugFPS = false;
/** @type boolean */
let debugSlerp = false;

//
// Stuff for FPS calculations.
//

/** @type number */
const maxPerfSamples = 32;
/** @type Float32Array */
const timeDeltaSamples = new Float32Array(maxPerfSamples);
/** @type Float32Array */
const timeUsedSamples = new Float32Array(maxPerfSamples);
/** @type number */
let numPerfSamples = 0;
/** @type string */
let fpsText = '';
/** @type number */
let fpsTextUpdated = document.timeline.currentTime;
/** @type DOMHighResTimeStamp */
let frameStartTime;

//|Inconsistent: All the variables to do with FPS data are separate globals. We should probably put them into an object, like we do for the slerp points.
/** @type {{A?: Vec2, B?: Vec2, C?: Vec2}} */
const debugSlerpInfo = {};

//
// Functions!
//

const $ = document.querySelector.bind(document);
const $$ = document.querySelectorAll.bind(document);

/**
 * <3 JavaScript
 * @type {(a: any, b: any) => boolean}
 */
function equals(a, b) {
    if (a === b)  return true;
    if (typeof a !== typeof b)  return false;
    if (typeof a !== 'object')  return false;
    if (Object.keys(a).length !== Object.keys(b).length)  return false;
    for (const key of Object.keys(a)) {
        if (!equals(a[key], b[key]))  return false;
    }
    return true;
}

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

/**
 * @type {(start: Transform, end: Transform, t: number) => Transform}
 */
function interpolateTransform(start, end, t) {
    //
    // Our goal is to transition the map from one state to another in a way that looks smooth.
    // To see how we achieve this, imagine drawing a triangle ABC on the screen.
    // Point A is at the origin in the top-left corner.
    // Now say the animation is about to start.
    // Find the map coordinate currently at point A.
    // Then put the map into its final state and see where that coordinate ends up---that's point B.
    // Find point C such that:
    // - the angle inside the triangle at point C is equal to the rotation being applied in the animation, and
    // - the ratio of the lengths of the sides AC and BC is equal to the scale being applied in the animation.
    // You make it look smooth by ensuring that point C doesn't move for the duration of the animation.
    // (To see what's going on, this triangle gets drawn on the screen if debugSlerp is true.)
    //
    if ((start.scale == end.scale) && (start.rotate == end.rotate)) {
        // The interpolation does not change scale or rotation. We will skip most of the computations and return early.
        // This isn't for speed. We are actually handling two cases where this function doesn't work:
        // 1. The start and end states are the same. In this case the triangle ABC is a single point.
        // 2. The start and end states only differ by translation. In this case there is no point C because every vector changes.
        const scale  = start.scale;
        const rotate = start.rotate;

        const translateX = lerp(start.translateX, end.translateX, t);
        const translateY = lerp(start.translateY, end.translateY, t);

        return {scale, rotate, translateX, translateY};
    }

    // For a zoom to look linear, it has to happen at an exponential rate with respect to the scale.
    // That is, consider the scales at the start and end of the animation as powers of two, and linearly interpolate the powers.
    const exp0  = Math.log2(start.scale);
    const exp1  = Math.log2(end.scale);
    const exp   = lerp(exp0, exp1, t);
    const scale = Math.pow(2, exp);

    const scaleRatio = end.scale/start.scale;

    let drot = end.rotate - start.rotate;
    if (drot < -Math.PI)       drot += 2*Math.PI; // Take the shortest arc.
    else if (drot > Math.PI)   drot -= 2*Math.PI;

    const rotate = lerp(start.rotate, start.rotate+drot, t);

    const gamma = Math.abs(drot);

    // Get the screen's top-left corner, at the start of the animation, in map coordinates.
    const p0 = inverseXform(start, {x:0, y:0});
    // Find out where that position on the map ends up, in screen coordinates, at the end of the animation.
    const p1 = xform(end, p0);

    //|Cleanup: wouldn't the below be better if we assumed b was unit-length instead (then we could multiply by scaleRatio instead of dividing).

    // We don't know the lengths of AC and BC, but we know their ratio. So solve the triangle by pretending BC is unit-length.
    // Reference for the law of cosines formulas used:
    // https://en.wikipedia.org/wiki/Solution_of_triangles#Two_sides_and_the_included_angle_given_(SAS)
    let a = 1;
    let b = a/scaleRatio;
    let c = Math.sqrt(Math.abs(a*a + b*b - 2*a*b*Math.cos(gamma))); // We use abs() because floating-point imprecision means this could otherwise try to get the square root of a negative number.

    if (c == 0) {
        // I think we should only get here if the start transform is the same as the end transform. They must
        // differ slightly due to floating point error, or we would have noticed that they were the same at the
        // top of this function, but they're too close to calculate a triangle. It would be nice to have a
        // sameTransform() function that detects this, but I'm not sure how to choose the epsilon correctly.
        return end;
    }

    let alpha = Math.acos(Math.max(-1, Math.min(1, (b*b + c*c - a*a)/(2*b*c)))); // The min/max stuff here is also due to floating-point imprecision, and the fact that acos() is only defined for [-1,1].
    if (drot > 0)  alpha *= -1;

    // Now that we've solved the triangle, scale the lengths according the one we actually know, which is AB.
    const actualc = Math.hypot(p1.x, p1.y);
    a *= actualc/c;
    b *= actualc/c;
    c  = actualc;

    // Turn the AB vector into the AC vector. |Cleanup: We should probably factor the maths below into functions like scaleVec and rotateVec.

    // Normalise AB. c must be actualc!
    const p2 = {x: p1.x/c, y: p1.y/c};
    // Rotate it.
    const p3 = {x: p2.x*Math.cos(alpha) - p2.y*Math.sin(alpha), y: p2.x*Math.sin(alpha) + p2.y*Math.cos(alpha)};
    // Make it the correct length.
    const p4 = {x: b*p3.x, y: b*p3.y};
    // Put it into map coordinates.
    const p5 = inverseXform(end, p4);

    const newTransform = {scale, rotate, translateX:0, translateY:0};

    const correction = xform(newTransform, p5);
    newTransform.translateX += p4.x - correction.x;
    newTransform.translateY += p4.y - correction.y;

    if (debugSlerp)  Object.assign(debugSlerpInfo, {A: {x:0, y:0}, B: p1, C: p4});

    return newTransform;
}

function copy(object) {
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
 * @type {(a: Box, b: Box) => boolean}
 */
function boxesTouch(a, b) {
    if (a[1].x < b[0].x)  return false;
    if (a[0].x > b[1].x)  return false;
    if (a[1].y < b[0].y)  return false;
    if (a[0].y > b[1].y)  return false;
    if (b[1].x < a[0].x)  return false;
    if (b[0].x > a[1].x)  return false;
    if (b[1].y < a[0].y)  return false;
    if (b[0].y > a[1].y)  return false;

    return true;
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
    const leftRect  = copy(rect);
    const remainder = copy(rect);

    leftRect.width   = width;
    remainder.x     += width;
    remainder.width -= width;

    return [leftRect, remainder];
}

/**
 * @type {(rect: Rect, width: number) => [Rect, Rect]}
 */
function cutRight(rect, width) {
    const rightRect = copy(rect);
    const remainder = copy(rect);

    remainder.width -= width;
    rightRect.x     += remainder.width;
    rightRect.width  = width;

    return [rightRect, remainder];
}

async function loadFonts() {
    const fonts = {
        "map-electorate":       "../fonts/RadioCanada.500.80.woff2",
        "title":                "../fonts/RadioCanada.500.90.woff2",
        "title-condensed":      "../fonts/RadioCanada.400.80.woff2",
        "button-active":        "../fonts/RadioCanada.700.90.woff2",
        "button-inactive":      "../fonts/RadioCanada.300.90.woff2",
        "chart-title":          "../fonts/RadioCanada.700.80.woff2",
        "chart-label":          "../fonts/RadioCanada.500.90.woff2",
    };

    const promises = {};

    for (const name of Object.keys(fonts)) {
        const url = fonts[name];

        if (!promises[url]) {
            promises[url] = fetch(url).then(response => response.arrayBuffer());
        }

        promises[url].then(data => {
            const fontFace = new FontFace(name, data);

            fontFace.load().then(f => document.fonts.add(f));
        });
    }
}

// gl must be initialised first. This function initialises program, u_proj and u_view.
function initWebGLProgram() {
    program = gl.createProgram();

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

    u_proj = gl.getUniformLocation(program, "u_proj"); // Transforms pixel space to UV space. Only changes when the screen dimensions change.
    u_view = gl.getUniformLocation(program, "u_view"); // Applies current pan/zoom. User-controlled.
}

function initInput() {
    window.addEventListener("pointerdown", event => {
        for (let i = 0; i < 2; i++) {
            const ptr = input.pointers[i];

            if (ptr.down)  continue;

            ptr.id = event.pointerId;
            ptr.x  = event.clientX;
            ptr.y  = event.clientY;
            ptr.active  = true;
            ptr.down    = true;
            ptr.pressed = true;

            return;
        }
    });

    window.addEventListener("pointerup", event => {
        for (let i = 0; i < 2; i++) {
            const ptr = input.pointers[i];

            if (ptr.id !== event.pointerId)  continue;

            ptr.x = event.clientX;
            ptr.y = event.clientY;
            ptr.active = true;

            if (ptr.down) {
                ptr.released = true;
                ptr.down = false;
            }

            return;
        }
    });

    window.addEventListener("pointermove", event => {
        const [p0, p1] = input.pointers;

        if (event.pointerId === p0.id || (!p0.down && !p1.down)) {
            p0.id = event.pointerId;
            p0.x  = event.clientX;
            p0.y  = event.clientY;
            p0.active = true;
        } else if (event.pointerId === p1.id) {
            p1.id = event.pointerId;
            p1.x  = event.clientX;
            p1.y  = event.clientY;
            p1.active = true;
        }
    });

    window.addEventListener("pointerout", event => {
        if (event.pointerId === input.pointers[0].id) {
            input.pointers[0].active = false;
        } else if (event.pointerId === input.pointers[1].id) {
            input.pointers[1].active = false;
        }
    });

    window.addEventListener("wheel", event => {
        input.scroll = event.deltaY;
    }, {passive: true});

    window.addEventListener("keydown", event => {
        if (event.key == "Shift") {
            input.shift = true;
        } else {
            input.keysPressed[event.code] = true;
        }
    });

    window.addEventListener("keyup", event => {
        if (event.key == "Shift") {
            input.shift = false;
        }
    });
}

/** @type {(layer: number, rect: Rect) => void} */
function addOcclusion(layer, rect) {
    occlusions[1].push({layer, rect});
}

// To be called once per frame.
function resetInput() {
    occlusions[0] = occlusions[1];
    occlusions[1] = [];

    for (let i = 0; i < 2; i++) {
        const ptr       = input.pointers[i];
        const lastPress = input.pointersLastPress[i];

        if (ptr.pressed) {
            lastPress.time = currentTime;
            lastPress.x    = ptr.x;
            lastPress.y    = ptr.y;
        }

        ptr.pressed  = false;
        ptr.released = false;
    }

    input.scroll = 0;
    input.keysPressed = {};
}

/**
 * @typedef {{pressed: boolean, hover: boolean, tapped: boolean}} PointerFlags
 *
 * @type {(rect: Rect, layer: number) => [PointerFlags, PointerFlags]}
 */
function getPointerFlags(rect, layer) {
    /** @type [PointerFlags, PointerFlags] */
    const result = [
        {pressed: false, hover: false, tapped: false},
        {pressed: false, hover: false, tapped: false}
    ];

    for (let i = 0; i < 2; i++) {
        const ptr = input.pointers[i];

        // Skip checking pointer events if the pointer is inactive unless it has just been released.
        // (On touchscreens, the pointer will be set to inavtive on the same frame it's released.)
        if (!ptr.active && !ptr.released)  continue;

        if (!pointInRect(ptr, rect))  continue;

        let occluded = false;

        for (let j = occlusions[0].length-1; j >= 0; j--) {
            if (occlusions[0][j].layer == layer)  break;

            if (pointInRect(ptr, occlusions[0][j].rect)) {
                occluded = true;
                break;
            }
        }

        if (!occluded) {
            if (ptr.released) {
                const lastPress = input.pointersLastPress[i];
                const downTime  = currentTime - lastPress.time;

                const maxTapTime = 1000;
                const maxDrag    = 10;
                if (downTime < maxTapTime) {
                    const drag = Math.hypot(ptr.x-lastPress.x, ptr.y-lastPress.y);
                    if (drag < maxDrag)  result[i].tapped = true;
                }
            }

            if (ptr.pressed)     result[i].pressed = true;
            else if (!ptr.down)  result[i].hover   = true;
        }
    }

    return result;
}

function handleUserEventsOnMap() {
    //
    // Handle mouse/touch events on the map.
    //
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

        const exp0 = Math.log2(minScale);
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

        const newTransform = copy(ct);
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
            start:     copy(ct),
            end:       newTransform,
            scroll:    true, // A special flag just for scroll-zoom animations, so we know we can trust map.scrollOffset.
        });
    }

    // Handle keyboard presses.
    {
        // Press shift and a number to save the map's current transformation. Then press just the number to return to it later.
        for (const key of Object.keys(input.keysPressed)) {
            const match = key.match(/^(Digit|Numpad)(\d)$/);
            if (!match)  continue;

            const saveKey = match[2];

            if (input.shift) {
                savedTransforms[saveKey] = copy(map.currentTransform);
            } else if (savedTransforms[saveKey]) {
                // We are going to transition to one of the saved transforms.
                const newTransform = copy(savedTransforms[saveKey]);

                // It will be a simple transition if there is any overlap between the map's bounding boxes before and after the transition.
                const box0 = getEnvelope(getMapCorners(map.width, map.height, map.currentTransform));
                const box1 = getEnvelope(getMapCorners(map.width, map.height, newTransform));
                const simple = boxesTouch(box0, box1);

                if (simple) {
                    const duration = 1000;

                    map.animations.length = 0;
                    map.animations.push({
                        startTime: currentTime,
                        endTime:   currentTime + duration,
                        start:     copy(map.currentTransform),
                        end:       newTransform,
                    });
                } else {
                    const durations = [750, 750];

                    /** @type Box */
                    const screen = [{x: 0, y: 0}, {x: map.width, y: map.height}];

                    //|Todo: It would be better if the mid-point was halfway between the start and end in terms of rotation. At the moment, if both start and end have a non-zero rotation---even if they're the same rotation---this two-part transition will go to 0 degrees in the middle, resulting in a near-360 in some cases.
                    //| I also think it may look smoother if, rather than each part having a hard-coded equal duration, the relative durations depended on the relative change in each part of the scale's exponent.
                    const midTransform = fitBox(combineBoxes(box0, box1), screen);

                    map.animations.length = 0;
                    map.animations.push({
                        startTime: currentTime,
                        endTime:   currentTime + durations[0],
                        start:     copy(map.currentTransform),
                        end:       midTransform,
                    });
                    map.animations.push({
                        startTime: currentTime + durations[0],
                        endTime:   currentTime + durations[0] + durations[1],
                        start:     midTransform,
                        end:       newTransform,
                    });
                }
            }
        }

        if (input.keysPressed['KeyE']) { //|Temporary
            // Just rotate the map by 90 degrees, to help test rotation animations.
            const corners = getMapCorners(map.width, map.height, map.currentTransform);

            const newTransform = copy(map.currentTransform);

            newTransform.rotate += Math.PI/2;

            // If we applied the newTransform as-is, get the would-be screen coordinates of what used to be the top-left corner.
            const screenCoords = xform(newTransform, corners[0]);
            // And put it into the bottom-left corner.
            newTransform.translateX -= screenCoords.x;
            newTransform.translateY += map.height - screenCoords.y;

            const duration = 1000;

            map.animations.length = 0;
            map.animations.push({
                startTime: currentTime,
                endTime:   currentTime + duration,
                start:     copy(map.currentTransform),
                end:       newTransform,
            });
        }

        // Check whether developer visualisations have been toggled:
        if (input.keysPressed['KeyT'])  debugTransform = !debugTransform;
        if (input.keysPressed['KeyL'])  debugLabels    = !debugLabels;
        if (input.keysPressed['KeyF'])  debugFPS       = !debugFPS;
        if (input.keysPressed['KeyS'])  debugSlerp     = !debugSlerp;
    }
}

async function maybeFetchVertices() {
    if (isVerticesRequestPending)  return;

    const election = elections[currentElectionIndex];

    if (!tileStore[election.id])  tileStore[election.id] = {};
    const dynamicTileset = tileStore[election.id];

    let scaleExp = Math.log2(map.currentTransform.scale);
    scaleExp = Math.round(scaleExp); //|Todo: Allow rounding to a finer gradient than whole numbers.

    // UPP: Map units per pixel. Increases as you zoom out.
    const upp = Math.pow(2, -scaleExp);

    if (!dynamicTileset[upp])  dynamicTileset[upp] = {};
    const tileset = dynamicTileset[upp];

    const pixelsPerTile = 512;
    const tileSize = pixelsPerTile*upp;

    /** @type Tileset */
    const subset = {};
    {
        const [min, max] = getEnvelope(getMapCorners(map.width, map.height, map.currentTransform));

        const minX = tileSize*Math.floor(min.x/tileSize);
        const minY = tileSize*Math.floor(min.y/tileSize);
        const maxX = tileSize*Math.ceil(max.x/tileSize);
        const maxY = tileSize*Math.ceil(max.y/tileSize);

        // The while loop below is equivalent to a nested for loop of the form:
        //  for (let x = minX; x <= maxX; x += tileSize) {
        //      for (let y = minY; y <= maxY; y += tileSize) {
        //      }
        //  }
        // We made it unnested so we could break out of it. Then we ended up returning instead of breaking out... |Cleanup.
        let x = minX;
        let y = minY;
        while (x <= maxX) {
            const key = x + ',' + y;

            if (!tileset[key]) {
                // We don't have all the tiles we need.
                isVerticesRequestPending = true;

                let url = '/vertices';
                url += '?';
                url += '&x0=' + x;
                url += '&y0=' + y;
                url += '&x1=' + (x + tileSize);
                url += '&y1=' + (y + tileSize);
                url += '&upp=' + upp;
                url += '&election=' + election.id;

                const response = await fetch(url);
                const data = await response.arrayBuffer();

                tileset[key] = new Float32Array(data);

                isVerticesRequestPending = false;

                return;
            }

            subset[key] = tileset[key];

            y += tileSize;
            if (y <= maxY)  continue;
            y = minY;
            x += tileSize;
        }
    }

    // We have all the tiles we need.

    if (equals(subset, currentTileset))  return;

    let totalNumFloats = 0;
    for (const key of Object.keys(subset))  totalNumFloats += subset[key].length;

    vertices = new Float32Array(totalNumFloats);
    {
        let i = 0;
        for (const key of Object.keys(subset)) {
            vertices.set(subset[key], i);
            i += subset[key].length;
        }
    }
    updateVertices = true;

    currentTileset = subset;
}

async function maybeFetchData() {
    const election = elections[currentElectionIndex];

    if (!election.districts) {
        election.districts = {};  // This makes this function idempotent.

        const response = await fetch(`/elections/${election.id}/districts.json`);
        const data     = await response.json();

        Object.assign(election.districts, data);
    }
}

function applyAnimationsToMap() {
    while (map.animations.length) {
        const {startTime, endTime, start, end} = map.animations[0];

        if (currentTime < startTime)  break;

        if (currentTime < endTime) {
            const t = (currentTime - startTime)/(endTime - startTime);

            map.currentTransform = interpolateTransform(start, end, t);

            break; // The first animation is ongoing, so we don't need to check the next one.
        }

        // The first animation has completed.
        Object.assign(map.currentTransform, end);

        map.animations.shift(); // Remove the first animation (and check the next one).
    }
}

function drawWebGL() {
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

    const canvas = /**@type HTMLCanvasElement*/(gl.canvas);
    canvas.width        = Math.ceil(dpr*map.width);
    canvas.height       = Math.ceil(dpr*map.height);
    canvas.style.width  = map.width + 'px';
    canvas.style.height = map.height + 'px';


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

function drawLabels() {
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

    const election = elections[currentElectionIndex];

    const districts = election.districts || {};

    //
    // Draw the labels.
    //
    for (const districtID of Object.keys(districts)) {
        /** @type District */
        const district = districts[districtID];

        const labelText = district.name.toUpperCase();
        const {width} = ui.measureText(labelText);
        const screenPos = xform(map.currentTransform, district.centroid);
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

        const labelRect = {x: textX, y: textY, width: textX1-textX, height: textY1-textY};

        let textColour    = 'white';
        let outlineColour = 'black';
        let hoverOutline  = 'grey';

        const [flags] = getPointerFlags(labelRect, Layer.MAP);

        if (flags.hover)  outlineColour = hoverOutline;

        if (flags.tapped) {
            currentDistrictID = +districtID; //|Jank: The districtID is already a number but we cast to appease tsc.

            const duration = 750;

            /** @type Box */
            const mapBox = [{x: 0, y: 0}, {x: map.width, y: map.height}];
            const newTransform = fitBox(district.box, mapBox);

            map.animations.length = 0;
            map.animations.push({
                startTime: currentTime,
                endTime:   currentTime + duration,
                start:     copy(map.currentTransform),
                end:       newTransform
            });
        }

        ui.strokeStyle = outlineColour;
        ui.lineWidth = 3;
        ui.strokeText(labelText, textX, textY);
        ui.fillStyle = textColour;
        ui.fillText(labelText, textX, textY);
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

/**
 * @typedef {{label: string, colour: string, value: number}} BarChartData
 * @typedef {{targetValue?: number}} BarChartConfig
 *
 * This function puts the actual chart height in rect.height.
 *
 * @type {(rect: Rect, title: string, bars: BarChartData[], config?: BarChartConfig) => void}
 */
function drawBarChart(rect, title, bars, config={}) {
    const {targetValue} = config;

    // We can make these configurable later if we need to.
    const titleHeight    = 15;
    const barHeight      = 17;
    const gapHeight      =  3; // This is used both under the title and between bars.
    const titleColour    = "black";
    const barLabelColour = "white";
    const targetColour   = "grey";

    // Computed variables:
    const barTextHeight  = Math.floor(0.75*barHeight);
    const barTextPadding = Math.ceil((barHeight - barTextHeight)/2); // ceil() because this will be added to the y value at the top of the bar. So if there's an extra pixel it gets added to the top.
    const showTarget     = targetValue != undefined;

    let maxValue = 0;
    if (showTarget)  maxValue = targetValue;
    for (const bar of bars) {
        if (maxValue < bar.value)  maxValue = bar.value;
    }
    if (maxValue == 0)  maxValue = 1; // Avoid dividing by zero.

    //
    // Draw!
    //
    let y = rect.y;

    // Draw the dashed line for the target. |Todo: Label the dashed line.
    if (showTarget) {
        ui.beginPath();
        ui.setLineDash([3, 2]);
        const targetX = rect.x + (targetValue/maxValue)*rect.width;
        ui.moveTo(targetX, rect.y);
        const bottomY = rect.y + titleHeight + bars.length*(gapHeight + barHeight);
        ui.lineTo(targetX, bottomY);
        ui.strokeStyle = targetColour;
        ui.lineWidth = 1;
        ui.stroke();
    }

    // Draw the title.
    ui.font = titleHeight + 'px chart-title';
    ui.fillStyle = titleColour;
    const titleWidth = ui.measureText(title).width;
    const titleX = rect.x + rect.width/2 - titleWidth/2;
    ui.fillText(title, titleX, y);

    y += titleHeight + gapHeight;

    for (let barIndex = 0; barIndex < bars.length; barIndex++) {
        if (barIndex > 0)  y += gapHeight;

        const bar = bars[barIndex];

        // Draw the bar.
        const barWidth = (bar.value/maxValue)*rect.width;

        ui.fillStyle = bar.colour;
        ui.fillRect(rect.x, y, barWidth, barHeight);

        // Draw the two bar labels:
        // 1. The name of the bar on the left.
        ui.font = barTextHeight + 'px chart-label';
        const labelWidth = ui.measureText(bar.label).width;
        const labelFits = labelWidth <= barWidth - 2*barTextPadding;
        if (labelFits) {
            // Put the label on the bar.
            ui.fillStyle = barLabelColour;
            ui.fillText(bar.label, rect.x + barTextPadding, y + barTextPadding);
        } else {
            // Put the label to the right of the bar.
            ui.fillStyle = titleColour;
            ui.fillText(bar.label, rect.x + barWidth + barTextPadding, y + barTextPadding);
        }

        // 2. The value of the bar on the right.
        const valueText = '' + bar.value;
        const valueWidth = ui.measureText(valueText).width;
        const valueFits = valueWidth <= barWidth - 4*barTextPadding - labelWidth;
        if (valueFits) {
            // Put the value on the bar.
            ui.fillStyle = barLabelColour;
            const valueX = rect.x + barWidth - barTextPadding - valueWidth;
            ui.fillText(valueText, valueX, y + barTextPadding);
        } else {
            // Put the value next to the bar.
            let valueX = rect.x + barWidth + barTextPadding;
            if (!labelFits)  valueX += labelWidth + 2*barTextPadding;
            ui.fillStyle = titleColour;
            ui.fillText(valueText, valueX, y + barTextPadding);
        }

        y += barHeight;
    }

    rect.height = y - rect.y;
}

function drawPanel() {
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
                    const alwaysShow = 20; // Don't let the user drag the panel out of sight---always show at least this many pixels.

                    const minY = document.body.clientHeight - panelRect.height;
                    const maxY = document.body.clientHeight - alwaysShow;

                    let dy = input.pointers[0].y - panelDragStartY;

                    if (panelRect.y + dy < minY)       dy = minY - panelRect.y;
                    else if (panelRect.y + dy > maxY)  dy = maxY - panelRect.y;

                    panelRect.y     += dy;
                    panelDragStartY += dy;
                }
            } else if (mobileMode) {
                //|Todo: cutTop
                const dragRect  = copy(panelRect);
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

    // Draw the election year switcher.
    {
        ui.save();

        const election = elections[currentElectionIndex];

        let isTitle = (currentDistrictID < 0);
        const textHeight = (isTitle) ? 30 : 15;

        const minButtonSize = textHeight;
        const maxTextWidth = panelWidth - 2*minButtonSize;
        const electionYear = '' + election.date.getFullYear();

        let leftButtonRect, rightButtonRect;

        // Try drawing '<year> Federal Election' on one line. If there isn't room, just draw the year.
        {
            let text = electionYear + ' Federal Election';

            if (isTitle) {
                ui.font = textHeight + 'px title';
            } else {
                text = text.toUpperCase();
                ui.font = textHeight + 'px button-inactive';
            }

            let textWidth = ui.measureText(text).width;

            let onlyYearDrawn = false;

            if (textWidth > maxTextWidth) {
                text = electionYear;
                textWidth = ui.measureText(text).width;
                onlyYearDrawn = true;
            }

            let textX = panelX + panelWidth/2 - textWidth/2;

            // If we're focused on a particular district, you can click on the election title to return to the default mode.
            if (currentDistrictID > 0) {
                const titleRect = {x:textX, y:panelY, width:textWidth, height:textHeight};

                const [flags] = getPointerFlags(titleRect, Layer.PANEL);

                // Make the text bold if the user is hovering over it.
                if (flags.hover) {
                    ui.font = textHeight + 'px button-active';
                    textWidth = ui.measureText(text).width;
                    textX = panelX + panelWidth/2 - textWidth/2;
                }

                if (flags.tapped)  currentDistrictID = -1;
            }

            // Draw the text.
            ui.fillStyle = 'black';
            ui.fillText(text, textX, panelY);

            const unusedWidth = panelWidth - textWidth;
            leftButtonRect  = {x: panelX,            y: panelY, width: unusedWidth/2, height: minButtonSize};
            rightButtonRect = {x: textX + textWidth, y: panelY, width: unusedWidth/2, height: minButtonSize};

            panelY += textHeight;

            // If it's the title, draw "Federal Election" on a line below if we didn't have room on one line.
            if (isTitle && onlyYearDrawn) {
                const textHeight = 20;
                const text = 'Federal Election';
                ui.font = textHeight + 'px title';
                const textWidth = ui.measureText(text).width;

                const textX = panelX + panelWidth/2 - textWidth/2;
                ui.fillText(text, textX, panelY);

                panelY += textHeight;

                // Also push the buttons down if there is room.
                const unusedWidth = panelWidth - textWidth;
                if (unusedWidth > 2*minButtonSize) {
                    leftButtonRect.width    = unusedWidth/2;
                    leftButtonRect.height  += textHeight;

                    rightButtonRect.x       = textX + textWidth;
                    rightButtonRect.width   = unusedWidth/2;
                    rightButtonRect.height += textHeight;
                }
            }
        }

        // Draw the buttons.
        {
            const normalColour   = "black";
            const disabledColour = "lightgrey";

            const animationDuration   = 250; // In milliseconds.
            const animationStartShade = 150; // Out of 255, as in a shade of grey.

            const iconHeight = 0.5*minButtonSize;
            const iconWidth  = iconHeight/2;

            // The left one.
            {
                let colour    = normalColour;
                let lineWidth = 0.2*iconHeight;

                if (currentElectionIndex == 0) {
                    colour = disabledColour;
                } else {
                    const [flags] = getPointerFlags(leftButtonRect, Layer.PANEL);

                    if (flags.hover)  lineWidth = 0.4*iconHeight;

                    if (flags.pressed) {
                        currentElectionIndex -= 1;

                        prevElectionButtonLastPressed = currentTime;
                    }
                }

                const timeSincePressed = currentTime - prevElectionButtonLastPressed;
                if (timeSincePressed < animationDuration) {
                    const shade = lerp(animationStartShade, 255, timeSincePressed/animationDuration);

                    ui.fillStyle = `rgb(${shade}, ${shade}, ${shade})`;
                    ui.fillRect(leftButtonRect.x, leftButtonRect.y, leftButtonRect.width, leftButtonRect.height);
                }

                let x = leftButtonRect.x + leftButtonRect.width/2 - iconWidth/2;
                let y = leftButtonRect.y + leftButtonRect.height/2 - iconHeight/2;

                ui.beginPath();
                ui.moveTo(x + iconWidth, y);
                ui.lineTo(x,             y + iconHeight/2);
                ui.lineTo(x + iconWidth, y + iconHeight);

                ui.lineWidth = lineWidth;
                ui.strokeStyle = colour;
                ui.stroke();
            }

            // The right one.
            {
                let colour    = normalColour;
                let lineWidth = 0.2*iconHeight;

                if (currentElectionIndex == elections.length-1) {
                    colour = disabledColour;
                } else {
                    const [flags] = getPointerFlags(rightButtonRect, Layer.PANEL);

                    if (flags.hover)  lineWidth = 0.4*iconHeight;

                    if (flags.pressed) {
                        currentElectionIndex += 1;

                        nextElectionButtonLastPressed = currentTime;
                    }
                }

                const timeSincePressed = currentTime - nextElectionButtonLastPressed;
                if (timeSincePressed < animationDuration) {
                    const shade = lerp(animationStartShade, 255, timeSincePressed/animationDuration);

                    ui.fillStyle = `rgb(${shade}, ${shade}, ${shade})`;
                    ui.fillRect(rightButtonRect.x, rightButtonRect.y, rightButtonRect.width, rightButtonRect.height);
                }

                let x = rightButtonRect.x + rightButtonRect.width/2 - iconWidth/2
                let y = rightButtonRect.y + rightButtonRect.height/2 - iconHeight/2;

                ui.beginPath();
                ui.moveTo(x,             y);
                ui.lineTo(x + iconWidth, y + iconHeight/2);
                ui.lineTo(x,             y + iconHeight);

                ui.lineWidth = lineWidth;
                ui.strokeStyle = colour;
                ui.stroke();
            }
        }

        ui.restore();
    }
    panelY += panelPadding;

    if (currentDistrictID < 0) {
        //
        // Election mode (we're looking at an election as a whole, no particular electorate).
        //

        // Draw a horizontal bar chart showing the results for the election as a whole.
        // |Todo: Factor this into a function that takes a config and rect and returns the chart height.
        {
            // Get data:
            const election = elections[currentElectionIndex];
            if (!election.seatsWon) {
                election.seatsWon = {alp: 0, lnp: 0, etc: 0};

                (async function(){ //|Cleanup: Overall this seems like it could be simpler. Maybe we should store the bars variable directly instead of having the intermediate seatsWon variable?
                    const response = await fetch(`/elections/${election.id}/seats-won.json`);
                    const data     = await response.json();

                    for (const party of data) {
                        switch (party.shortCode) {
                            case 'ALP':
                                election.seatsWon.alp += party.count;
                                break;
                            case 'LP':
                            case 'NP':
                            case 'LNP':
                                election.seatsWon.lnp += party.count;
                                break;
                            default:
                                election.seatsWon.etc += party.count;
                        }
                    }
                })();
            }
            const bars = [
                {label: "ALP",   colour: "#c31f2f", value: election.seatsWon.alp},
                {label: "LNP",   colour: "#19488f", value: election.seatsWon.lnp},
                {label: "Other", colour: "#808080", value: election.seatsWon.etc},
            ];

            let targetValue = 75;
            if (election.districts) {
                const numDistricts = Object.keys(election.districts).length;
                if (numDistricts)  targetValue = Math.ceil(numDistricts/2);
            }

            const chartRect = {x:panelX, y:panelY, width:panelWidth, height:0};

            drawBarChart(chartRect, "Seats won", bars, {targetValue});

            panelY = chartRect.y + chartRect.height;
        }
        panelY += panelPadding;
    } else {
        //
        // District mode (we're focused on one particular electorate).
        //

        const election = elections[currentElectionIndex];
        if (!election.districts || !Object.keys(election.districts).length) {
            // The districts for the current election haven't loaded yet. Pass.
        } else if (!election.districts[currentDistrictID]) {
            // The districts for the current election have loaded, but the previously-selected district ID
            // isn't in the currently-selected election. Revert to election mode.
            currentDistrictID = -1;
        } else {
            const district = election.districts[currentDistrictID];

            // Draw the district name.
            {
                const text = district.name.toUpperCase();
                const textHeight = 30;
                ui.font = textHeight + 'px title';
                let textWidth = ui.measureText(text).width;
                if (textWidth > panelWidth) {
                    ui.font = textHeight + 'px title-condensed';
                    textWidth = ui.measureText(text).width;
                }
                const textX = panelX + panelWidth/2 - textWidth/2;
                ui.fillStyle = 'black';
                ui.fillText(text, textX, panelY);

                panelY += textHeight;
            }
            panelY += panelPadding;

            // Make sure we have the data for the two-candidate-preferred and first-preferences charts.
            if (!district.votes) {
                district.votes = {
                    tcp: [
                        {firstName: "", lastName: "",  partyName: "",  partyCode: "", colour: "#808080", numVotes: 1, ballotPosition: 1},
                        {firstName: "", lastName: "",  partyName: "",  partyCode: "", colour: "#808080", numVotes: 1, ballotPosition: 2},
                    ],
                    fp: []
                };

                (async function(){
                    const response = await fetch(`/elections/${election.id}/contests/${currentDistrictID}/votes.json`);
                    const data     = await response.json();

                    district.votes = data;
                })();
            }

            // Draw the two-candidate-preferred chart.
            {
                const tcp = district.votes.tcp;

                // Chart config:
                const titleText = "Preference count";

                // Style constants:
                const titleHeight    = 15;
                const namesHeight    = 13;
                const barHeight      = 17;
                const gapHeight      =  5;

                const titleColour    = "black";
                const barLabelColour = "white";
                const targetColour   = "grey";

                // Computed variables:
                const totalVotes = tcp[0].numVotes + tcp[1].numVotes;

                //
                // Draw.
                //

                // Draw the title:
                {
                    ui.font = titleHeight + 'px chart-title';
                    const titleWidth = ui.measureText(titleText).width;
                    const titleX = panelX + panelWidth/2 - titleWidth/2;
                    ui.fillStyle = titleColour;
                    ui.fillText(titleText, titleX, panelY);
                }
                panelY += titleHeight + gapHeight;

                // Draw the dashed line at the 50% mark:
                {
                    ui.beginPath();
                    ui.setLineDash([3, 2]);
                    const targetX = panelX + panelWidth/2;
                    ui.moveTo(targetX, panelY);
                    const bottomY = panelY + 2*gapHeight + namesHeight + barHeight;
                    ui.lineTo(targetX, bottomY);
                    ui.strokeStyle = targetColour;
                    ui.lineWidth = 1;
                    ui.stroke();
                }

                // Draw the candidates' names.
                {
                    ui.font = namesHeight + 'px chart-label';
                    ui.fillStyle = titleColour;

                    // Check whether we can fit the full names of both candidates.
                    const fullNames    = tcp.map(c => c.firstName + ' ' + c.lastName);
                    const nameWidths   = fullNames.map(n => ui.measureText(n).width);
                    const fullNamesFit = panelWidth > (nameWidths[0] + nameWidths[1] + 2*gapHeight);

                    if (fullNamesFit) {
                        ui.fillText(fullNames[0], panelX, panelY);
                        ui.fillText(fullNames[1], panelX + panelWidth - nameWidths[1], panelY);
                    } else {
                        ui.fillText(tcp[0].lastName, panelX, panelY);
                        // Draw the right name.
                        const rightNameWidth = ui.measureText(tcp[1].lastName).width;
                        ui.fillText(tcp[1].lastName, panelX + panelWidth - rightNameWidth, panelY);
                    }
                }
                panelY += namesHeight + 1;

                // Draw the bars.
                {
                    const leftWidth = (tcp[0].numVotes/totalVotes)*panelWidth;
                    const rects = cutLeft({x:panelX, y:panelY, width:panelWidth, height:barHeight}, leftWidth);

                    for (let i = 0; i < 2; i++) {
                        const {x, y, width, height} = rects[i];

                        ui.fillStyle = tcp[i].colour;
                        ui.fillRect(x, y, width, height);
                    }
                }
                panelY += barHeight + gapHeight;
            }
            panelY += panelPadding;

            // Draw the first-preferences chart.
            {
                const fp = district.votes.fp;

                if (fp.length) {
                    const bars = fp.map(c => {
                        return {label: c.partyCode, colour: c.colour, value: c.numVotes};
                    });

                    const chartRect = {x:panelX, y:panelY, width:panelWidth, height:0};

                    drawBarChart(chartRect, "First preference", bars);

                    panelY = chartRect.y + chartRect.height;
                }
            }
        }
        panelY += panelPadding;
    }

    // For the next frame, set the panel's height to the used height.
    panelRect.height = panelY - panelRect.y;
}

function drawTransform() {
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

function drawFPS() {
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

function drawSlerp() {
    if (Object.keys(debugSlerpInfo).length == 0)  return;

    const {A, B, C} = debugSlerpInfo;

    ui.strokeStyle = 'white';
    ui.lineWidth   = 2;

    ui.moveTo(A.x, A.y);
    ui.lineTo(B.x, B.y);
    ui.lineTo(C.x, C.y);
    ui.lineTo(A.x, A.y);
    ui.stroke();

    for (const key of Object.keys(debugSlerpInfo)) {
        const p = debugSlerpInfo[key];

        ui.fillStyle = 'red';
        ui.fillRect(p.x-4, p.y-4, 8, 8);

        ui.font = `12px monospace`;
        ui.fillStyle = 'black';
        ui.fillText(key, p.x+1, p.y+1);
        ui.fillStyle = 'white';
        ui.fillText(key, p.x, p.y);
    }
}

function drawUI() {
    ui.canvas.width        = Math.ceil(dpr*map.width);
    ui.canvas.height       = Math.ceil(dpr*map.height);
    ui.canvas.style.width  = map.width + 'px';
    ui.canvas.style.height = map.height + 'px';

    ui.scale(dpr, dpr);
    ui.clearRect(0, 0, map.width, map.height);

    drawLabels();

    drawPanel();
}

//
// The main loop (runs once per frame).
//
function step(time) {
    //
    // Update state.
    //
    timeDelta = time - currentTime;
    currentTime = time;

    if (debugFPS)  frameStartTime = performance.now();

    map.width  = document.body.clientWidth+1;
    map.height = document.body.clientHeight+1;

    dpr = window.devicePixelRatio || 1;


    handleUserEventsOnMap();
    applyAnimationsToMap();

    // Enforce the range of the map's rotation.
    {
        const ct = map.currentTransform;
        if (ct.rotate < -Math.PI)       ct.rotate += 2*Math.PI;
        else if (ct.rotate >= Math.PI)  ct.rotate -= 2*Math.PI;
    }

    maybeFetchVertices();
    maybeFetchData();

    drawWebGL();
    drawUI();

    resetInput();

    if (debugTransform)  drawTransform();
    if (debugSlerp)      drawSlerp();
    if (debugFPS)        drawFPS();

    window.requestAnimationFrame(step);
}

document.addEventListener("DOMContentLoaded", async () => {
    loadFonts();
    initInput();

    gl = $("canvas#map").getContext("webgl");
    initWebGLProgram();

    ui = $("canvas#gui").getContext("2d");

    // When the page loads, fit Australia on the screen.
    {
        map.width  = document.body.clientWidth+1;
        map.height = document.body.clientHeight+1;

        /** @type Box */
        const aust = [{x:-1863361, y: 1168642}, {x: 2087981, y: 4840595}];
        /** @type Box */
        const screen = [{x: 0, y: 0}, {x: map.width, y: map.height}];

        map.currentTransform = fitBox(aust, screen);

        // Return to this state by pressing 1.
        savedTransforms[1] = copy(map.currentTransform);
    }

    step();
});
