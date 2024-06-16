/**
        @typedef {{
                scale:      number,
                rotate:     number,
                translateX: number,
                translateY: number
            }} Transform

        @typedef {{x: number, y: number}} Vec2

        @typedef {{
                x:      number,
                y:      number,
                width:  number,
                height: number
            }} Rect

    Box is a variable-length array of 2D points. In practice, it's 2 or 4 points,
    depending on whether we're taking the box's rotation into account.

        @typedef {Vec2[]} Box
 */

const $ = document.querySelector.bind(document);
const $$ = document.querySelectorAll.bind(document);

const VERTEX_SHADER_TEXT = `
    precision highp float;

    uniform mat3 u_proj;
    uniform mat3 u_view;

    attribute vec2 v_position;
    attribute vec4 v_colour;

    varying vec4 f_colour;

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

    varying vec4 f_colour;

    void main()
    {
        gl_FragColor = f_colour;
    }
`;

/** @type {(transform: Transform, vec2: Vec2) => Vec2} */
function xform(transform, vec2) {
    const {scale, rotate, translateX, translateY} = transform;
    const {x, y} = vec2;

    const sin = Math.sin(rotate);
    const cos = Math.cos(rotate);

    const x1 = scale*(x*cos + y*sin) + translateX;
    const y1 = scale*(y*cos - x*sin) + translateY;

    return {x: x1, y: y1};
}

/** @type {(transform: Transform, vec2: Vec2) => Vec2} */
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

/** @type {(a: number, b: number, t: number) => number} */
function lerp(a, b, t) {
    return (1-t)*a + t*b;
}

function clone(object) {
    return JSON.parse(JSON.stringify(object));
}

/** @type {(inner: Rect, outer: Rect) => Transform} */      // |Cleanup: Change to Box (array of 2)
function fitBox(inner, outer) {
// Return the transform (applied to the inner box) required to fit the inner box in the centre
// of the outer box. The boxes are of the form {x, y, width, height}.
    const transform = {scale: 1, rotate: 0, translateX: 0, translateY: 0};

    const innerRatio = inner.width/inner.height;
    const outerRatio = outer.width/outer.height;

    if (innerRatio < outerRatio)  transform.scale = outer.height/inner.height;
    else                          transform.scale = outer.width/inner.width;

    transform.translateX = -transform.scale*inner.x + (outer.width - transform.scale*inner.width)/2;
    transform.translateY = -transform.scale*inner.y + (outer.height - transform.scale*inner.height)/2;

    return transform;
}

/** @type {(a: Rect, b: Rect) => Rect} */   // |Cleanup: Change to Box (array of 2)
function combineBoxes(a, b) {
// If one of the boxes contains the other, return the larger box (the actual object, not a copy of it).
// |Cleanup: All this would be less tedious if we made common use of 'box' vs 'rect' and actually used boxes.
    const ax1 = a.x;
    const ay1 = a.y;
    const ax2 = a.x + a.width;
    const ay2 = a.y + a.height;

    const bx1 = b.x;
    const by1 = b.y;
    const bx2 = b.x + b.width;
    const by2 = b.y + b.height;

    if (ax1 < bx1) {
        if ((ay1 < by1) && (ax2 > bx2) && (ay2 > by2))  return a;
    } else {
        if ((by1 < ay1) && (bx2 > ax2) && (by2 > ay2))  return b;
    }

    const x = (ax1 < bx1) ? ax1 : bx1;
    const y = (ay1 < by1) ? ay1 : by1;

    const width  = ((ax2 > bx2) ? ax2 : bx2) - x;
    const height = ((ay2 > by2) ? ay2 : by2) - y;

    return {x, y, width, height};
}

document.addEventListener("DOMContentLoaded", async () => {
    /** @type WebGLRenderingContext    */ const gl = $("canvas#map").getContext("webgl");
    /** @type CanvasRenderingContext2D */ const ui = $("canvas#gui").getContext("2d");

    const program = gl.createProgram();
    {
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

        gl.vertexAttribPointer(v_position, 2, gl.FLOAT, false, 6*4, 0);
        gl.vertexAttribPointer(v_colour,   4, gl.FLOAT, false, 6*4, 8); // |Cleanup: Avoid hard-coding these numbers.
    }

    const u_proj = gl.getUniformLocation(program, "u_proj"); // Transforms pixel space to UV space. Only changes when the screen dimensions change.
    const u_view = gl.getUniformLocation(program, "u_view"); // Applies current pan/zoom. User-controlled.

    let vertices; {
        const response = await fetch("../bin/vertices");
        const data = await response.arrayBuffer();

        vertices = new Float32Array(data);
    }

    /** @typedef {{text: string, pos: [number, number]}} Label */
    /** @type {Label[]} */
    let labels; {
        const response = await fetch("../bin/labels.json");
        const json = await response.json();

        labels = json.labels;
    }

    //
    // Handle user input.
    //

    const input = {
        // pointer[0] is the mouse, or the first finger to touch the screen. pointer[1] is the second finger.
        /** @typedef {{id: number, down: boolean, x: number, y: number}} Pointer */
        /** @type {[Pointer, Pointer]} */
        pointers: [
            {id: 0, down: false, x: 0, y: 0}, // X and Y are in screen coordinates.
            {id: 0, down: false, x: 0, y: 0},
        ],

        /** @type {number} */
        scroll: 0, // The deltaY of wheel events.

        /** @type {{[key: string]: boolean}} */
        pressed: {}, // E.g. {a: true, b: true}.
    };

    window.addEventListener("pointerdown", event => {
        for (let i = 0; i < 2; i++) {
            const ptr = input.pointers[i];

            if (ptr.down)  continue;

            ptr.id   = event.pointerId;
            ptr.down = true;
            ptr.x    = event.clientX;
            ptr.y    = event.clientY;

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
        input.pressed[event.key] = true; // |Temporary.
    });

    const map = {
        //
        // Map constants:
        //
        minScale:  0.0001,
        maxScale:  0.5,
        maxScroll: 8000, // How far you have to scroll (in "pixels") to go from the minimum to maximum scale.

        //
        // Map state variables:
        //
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

    // Toggle developer visualisations. |Debug
    let debugTransform = false;
    let debugLabels    = false;
    let debugFPS       = true;

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

        const windowWidth  = document.body.clientWidth;
        const windowHeight = document.body.clientHeight;

        // Handle mouse/touch events on the map.
        {
            const ptr  = input.pointers;
            const lock = map.pointerLocks;
            const ct   = map.currentTransform;

            for (let i = 0; i < 2; i++) {
                if (ptr[i].down) {
                    if (!lock[i].locked) {
                        // The user has just pressed the mouse or touched the screen.
                        lock[i].locked = true;

                        const pointerMapCoords = inverseXform(ct, ptr[i]);

                        lock[i].x = pointerMapCoords.x;
                        lock[i].y = pointerMapCoords.y;

                        map.animations.length = 0; // Cancel any current animations.
                    }
                } else {
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

            // We've consumed the scroll! |Todo: Reset per frame.
            input.scroll = 0;
        }

        // Handle keyboard presses.
        {
            // |Temporary: When the user presses certain numbers, animate the map to show different locations.
            const aust = {x: -1863361, y: 1168642, width: 3951342, height: 3671953};
            const melb = {x:  1140377, y: 4187714, width:    8624, height:    8663};
            const syd  = {x:  1757198, y: 3827047, width:    5905, height:    7899};

            const boxes = {'0': aust, '1': melb, '2': syd};

            for (const key of Object.keys(boxes)) {
                if (input.pressed[key]) {
                    const targetBox = boxes[key];

                    /** @type Rect */ // |Cleanup: Change to Box.
                    let screenBounds; { // Get the screen in map coordinates.
                        const {x: x1, y: y1} = inverseXform(map.currentTransform, {x: 0, y: 0});
                        const {x: x2, y: y2} = inverseXform(map.currentTransform, {x: windowWidth, y: windowHeight});

                        screenBounds = {x: x1, y: y1, width: x2-x1, height: y2-y1};
                    }

                    // |Todo: Expand the combined box by 10%.
                    const combined = combineBoxes(targetBox, screenBounds);

                    // It's a simple transition if one of the boxes contains the other.
                    const simple = (combined === screenBounds) || (combined === screenBounds);

                    if (simple) {
                        const duration = 1000;

                        const screen = {x: 0, y: 0, width: windowWidth, height: windowHeight};
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

                        const screen = {x: 0, y: 0, width: windowWidth, height: windowHeight};
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

                    // We've consumed this key press. |Cleanup: Delete all these once per frame.
                    delete input.pressed[key];
                }
            }

            // Check whether developer visualisations have been toggled:
            if (input.pressed['t']) {
                debugTransform = !debugTransform;
                delete input.pressed['t'];
            }
            if (input.pressed['l']) {
                debugLabels = !debugLabels;
                delete input.pressed['l'];
            }
            if (input.pressed['f']) {
                debugFPS = !debugFPS;
                delete input.pressed['f'];
            }
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
            proj[0] =  2/windowWidth;     // X scale.
            proj[4] = -2/windowHeight;    // Y scale.
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
            canvas.width  = Math.floor(dpr*windowWidth);
            canvas.height = Math.floor(dpr*windowHeight);

            canvas.style.width  = windowWidth + "px";
            canvas.style.height = windowHeight + "px";
        });

        // WebGL canvas:
        {
            gl.viewport(0, 0, dpr*windowWidth, dpr*windowHeight);

            gl.clearColor(0.1, 0.1, 0.1, 1.0); // Background colour (same as water): off-black.
            gl.clear(gl.COLOR_BUFFER_BIT);

            gl.useProgram(program);

            gl.uniformMatrix3fv(u_proj, false, proj);
            gl.uniformMatrix3fv(u_view, false, view);

            // |Speed: We only want to re-buffer the vertices if they've changed.
            // This seems to be especially important for performance on Firefox.
            gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);

            gl.drawArrays(gl.TRIANGLES, 0, vertices.length/6); // 6 is the number of floats per vertex attribute.

            const error = gl.getError();
            if (error)  console.error(`WebGL error. Code: ${error}`);
        }

        // 2D canvas:
        {
            ui.scale(dpr, dpr);
            ui.clearRect(0, 0, windowWidth, windowHeight);

            //
            // Draw the labels from the JSON file, as stably as possible!
            //
            {
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
                const height = 16;
                ui.font = `${height}px sans serif`;
                ui.textBaseline = "top";

                const resolution = 512; // |Speed.
                const usedSpace  = new Int8Array(resolution);

                /** @type Box */
                const screenBoxScreenCoords = [
                    {x: 0,           y: 0},
                    {x: 0,           y: windowHeight},
                    {x: windowWidth, y: windowHeight},
                    {x: windowWidth, y: 0}
                ];
                /** @type Box */
                const screenBoxMapCoords = screenBoxScreenCoords.map(v => inverseXform(map.currentTransform, v));

                let gridSize, numGridCols, numGridRows, gridRect; {
                    // Find the map-axis-aligned rectangle enclosing the screen's bounding box:
                    let minX, minY, maxX, maxY; {
                        minX = maxX = screenBoxMapCoords[0].x;
                        minY = maxY = screenBoxMapCoords[0].y;
                        for (let i = 1; i < 4; i++) {
                            const {x, y} = screenBoxMapCoords[i];
                            if (x < minX)       minX = x;
                            else if (x > maxX)  maxX = x;
                            if (y < minY)       minY = y;
                            else if (y > maxY)  maxY = y;
                        }
                    }
                    const focusWidth  = maxX-minX;
                    const focusHeight = maxY-minY;

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
                        x:      minX - (minX % gridSize),
                        y:      minY - (minY % gridSize),
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
                    /** @type Box */
                    const box = [
                        {x: textX,  y: textY},
                        {x: textX,  y: textY1},
                        {x: textX1, y: textY1},
                        {x: textX1, y: textY}
                    ];
                    for (let i = 0; i < 4; i++)  box[i] = inverseXform(map.currentTransform, box[i]);

                    let minX, minY, maxX, maxY; {
                        minX = maxX = box[0].x;
                        minY = maxY = box[0].y;
                        for (let i = 1; i < box.length; i++) {
                            const {x, y} = box[i];
                            if (x < minX)       minX = x;
                            else if (x > maxX)  maxX = x;
                            if (y < minY)       minY = y;
                            else if (y > maxY)  maxY = y;
                        }
                    }

                    // Don't draw labels that are outside our grid:
                    if (minY > gridRect.y + gridRect.height)  continue;
                    if (maxY < gridRect.y)                    continue;
                    if (minX > gridRect.x + gridRect.width)   continue;
                    if (maxX < gridRect.x)                    continue;

                    // Check whether any of the grid squares we want have been taken.

                    let used = false;

                    const col0 = Math.floor((minX - gridRect.x)/gridSize);
                    const row0 = Math.floor((minY - gridRect.y)/gridSize);
                    const col1 = Math.ceil((maxX - gridRect.x)/gridSize);
                    const row1 = Math.ceil((maxY - gridRect.y)/gridSize);

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

                    // Once we fail once, skip subsequent labels.
                    if (used)  break;

                    // We are now going to use the space. Mark the squares as used.
                    for (let row = row0; row < row1; row++) {
                        for (let col = col0; col < col1; col++) {
                            const index = row*numGridCols + col;
                            usedSpace[index] = 1;
                        }
                    }

                    ui.fillStyle = 'white';
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

            if (debugTransform) { // Draw the map's current transform in the bottom-right corner of the canvas. |Debug
                const height = 16; // Text height.
                ui.font = height + 'px sans serif';
                ui.textBaseline = "top";

                let y = windowHeight - height;

                for (const target of ["translateY", "translateX", "rotate", "scale"]) {
                    const label = target + ': ' + map.currentTransform[target];
                    const width = 200;
                    const x     = windowWidth - width;

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
                    ui.font          = `${textHeight}px sans serif`;
                    ui.fillStyle     = 'white';
                    ui.textBaseline  = "top";

                    let x = 0;
                    let y = 0;
                    for (const line of fpsText.split('\n')) {
                        ui.fillText(line, x, y);
                        y += textHeight;
                    }
                }
            }
        }

        window.requestAnimationFrame(step);
    })();

    // When the page loads, fit Australia on the screen.
    {
        const aust   = {x: -1863361, y: 1168642, width: 3951342,                   height: 3671953};
        const screen = {x: 0,        y: 0,       width: document.body.clientWidth, height: document.body.clientHeight};

        map.currentTransform = fitBox(aust, screen);
    }

    // For debugging:
    Object.assign(window, {map, gl, ui});
});
