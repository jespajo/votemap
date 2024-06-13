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

function xform(transform, vec2) {
    const {scale, rotate, translateX, translateY} = transform;
    const [x0, y0] = vec2;

    const sin = Math.sin(rotate);
    const cos = Math.cos(rotate);

    const x1 = scale*(x0*cos + y0*sin) + translateX;
    const y1 = scale*(y0*cos - x0*sin) + translateY;

    return [x1, y1];
}

function inverseXform(transform, vec2) {
    const {scale, rotate, translateX, translateY} = transform;
    const [x0, y0] = vec2;

    const sin = Math.sin(rotate);
    const cos = Math.cos(rotate);

    // @Todo: Simpler way?!?!
    const det = scale*scale*(cos*cos + sin*sin);
    if (det == 0)  throw new Error();

    const x1 = x0 - translateX;
    const y1 = y0 - translateY;

    // @Cleanup: Redundant scale on numerator and denom.
    const x2 = (scale/det)*(x1*cos - y1*sin);
    const y2 = (scale/det)*(x1*sin + y1*cos);

    return [x2, y2];
}

function lerp(a, b, t) {
    return (1-t)*a + t*b;
}

function clone(object) {
    return JSON.parse(JSON.stringify(object));
}

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

function combineBoxes(a, b) {
// If one of the boxes contains the other, return the larger box (the actual object, not a copy of it).
// @Cleanup: All this would be less tedious if we made common use of 'box' vs 'rect' and actually used boxes.
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
    const gl = $("canvas#map").getContext("webgl");
    const ui = $("canvas#gui").getContext("2d");

    const program = gl.createProgram();
    {
        const vertexShader = gl.createShader(gl.VERTEX_SHADER)
        gl.shaderSource(vertexShader, VERTEX_SHADER_TEXT);
        gl.compileShader(vertexShader); // @Fixme: Check compilation errors.

        const fragmentShader = gl.createShader(gl.FRAGMENT_SHADER);
        gl.shaderSource(fragmentShader, FRAGMENT_SHADER_TEXT);
        gl.compileShader(fragmentShader); // @Fixme: Check compilation errors.

        gl.attachShader(program, vertexShader);
        gl.attachShader(program, fragmentShader);
        gl.linkProgram(program); // @Todo: Clean up shaders?
        if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
            console.error("Shader program did not link successfully.");
            console.error("Error log: ", gl.getProgramInfoLog(program));
            return; // @Todo: Clean up GL program?
        }

        const buffer = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, buffer);

        const v_position = gl.getAttribLocation(program, "v_position");
        const v_colour   = gl.getAttribLocation(program, "v_colour");

        gl.enableVertexAttribArray(v_position);
        gl.enableVertexAttribArray(v_colour);

        gl.vertexAttribPointer(v_position, 2, gl.FLOAT, false, 6*4, 0);
        gl.vertexAttribPointer(v_colour,   4, gl.FLOAT, false, 6*4, 8); // @Cleanup: Avoid hard-coding these numbers.
    }

    const u_proj = gl.getUniformLocation(program, "u_proj"); // Transforms pixel space to UV space. Only changes when the screen dimensions change.
    const u_view = gl.getUniformLocation(program, "u_view"); // Applies current pan/zoom. User-controlled.

    let vertices; {
        const response = await fetch("../bin/vertices");
        const data = await response.arrayBuffer();

        vertices = new Float32Array(data);
    }

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
        pointers: [
            {id: 0, down: false, x: 0, y: 0}, // X and Y are in screen coordinates.
            {id: 0, down: false, x: 0, y: 0},
        ],

        scroll: 0, // The deltaY of wheel events.

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
            ptr.x    = event.clientX; // @Cleanup: Necessary?
            ptr.y    = event.clientY; // @Cleanup: Necessary?

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
        input.pressed[event.key] = true; // @Temporary.
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
        currentTransform: {
            scale:      1,
            rotate:     0, // The angle, in radians, of a counter-clockwise rotation.
            translateX: 0,
            translateY: 0,
        },

        // When the user presses their mouse down on the map, we lock the mouse position to its
        // current location on the map. On a touchscreen, we do this with up to two fingers.
        pointerLocks: [  // X and Y are in map coordinates.
            {locked: false, x: 0, y: 0},
            {locked: false, x: 0, y: 0},
        ],

        // Members of the animations array look like this:
        //  {
        //      startTime:  <time stamp>
        //      endTime:    <time stamp>
        //      start:      {scale: 1, rotate: 0, translateX: 0, translateY: 0},
        //      end:        {scale: 3, rotate: 0, translateX: 0, translateY: 0},
        //      scroll?:    true,
        //  }
        animations: [],

        // This value represents how far the user has scrolled in "pixels", if you imagine the map
        // is a normal web page where, as you scroll, the map goes from minimum to maximum zoom.
        // This value is only valid if there is currently a scroll animation happening. Otherwise it
        // must be recalculated from map.currentTransform.scale.
        scrollOffset: 0,
    };

    // Toggle developer visualisations. @Debug
    let debugTransform = false;
    let debugLabels    = false;

    let currentTime = document.timeline.currentTime;

    //
    // The main loop (runs once per frame):
    //
    ;(function step(time) {
        //
        // Update state.
        //
        const timeDelta = time - currentTime;
        currentTime = time;

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

                        const pointerMapCoords = inverseXform(ct, [ptr[i].x, ptr[i].y]);

                        lock[i].x = pointerMapCoords[0];
                        lock[i].y = pointerMapCoords[1];

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
                // @Bug: The below calculations break when d == 0 and maybe other times?
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
                const lockScreenCoords = xform(ct, [lock[0].x, lock[0].y]);

                ct.translateX += ptr[0].x - lockScreenCoords[0];
                ct.translateY += ptr[0].y - lockScreenCoords[1];
            } else {
                for (let i = 0; i < 2; i++) {
                    if (lock[i].locked && !lock[1-i].locked) {
                        const lockScreenCoords = xform(ct, [lock[i].x, lock[i].y]);

                        ct.translateX += ptr[i].x - lockScreenCoords[0];
                        ct.translateY += ptr[i].y - lockScreenCoords[1];

                        break;
                    }
                }
            }
        }

        // Handle scroll.
        if (input.scroll) {
            const {minScale, maxScale, maxScroll} = map;
            const ct = map.currentTransform;

            const exp0 = Math.log2(minScale); // @Speed.
            const exp1 = Math.log2(maxScale); // @Speed.

            if (!map.animations.length || !map.animations[0].scroll) {
                // There is not currently a scroll animation happening, so we can't trust the
                // map.scrollOffset variable. Calculate it again based on the current scale.
                const exp = Math.log2(ct.scale); // @Fixme: What if this is outside our expected range?
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
            const origin = inverseXform(ct, [mouse.x, mouse.y]);

            // @Naming: These variables. Call them something like "error", "correction", "offset"?
            const originScreenCoords = xform(newTransform, origin);
            newTransform.translateX += mouse.x - originScreenCoords[0];
            newTransform.translateY += mouse.y - originScreenCoords[1];

            const duration = 100;

            map.animations.length = 0;
            map.animations.push({
                startTime: currentTime,
                endTime:   currentTime + duration,
                start:     clone(ct),
                end:       newTransform,
                scroll:    true, // A special flag just for scroll-zoom animations, so we know we can trust map.scrollOffset.
            });

            // We've consumed the scroll! @Todo: Reset per frame.
            input.scroll = 0;
        }

        // Handle keyboard presses.
        {
            // @Temporary: When the user presses certain numbers, animate the map to show different locations.
            const aust = {x: -1863361, y: 1168642, width: 3951342, height: 3671953};
            const melb = {x:  1140377, y: 4187714, width:    8624, height:    8663};
            const syd  = {x:  1757198, y: 3827047, width:    5905, height:    7899};

            const boxes = {'0': aust, '1': melb, '2': syd};

            for (const key of Object.keys(boxes)) {
                if (input.pressed[key]) {
                    const targetBox = boxes[key];

                    let screenBounds; { // Get the screen in map coordinates.
                        const [x1, y1] = inverseXform(map.currentTransform, [0, 0]);
                        const [x2, y2] = inverseXform(map.currentTransform, [windowWidth, windowHeight]);

                        screenBounds = {x: x1, y: y1, width: x2-x1, height: y2-y1};
                    }

                    // @Todo: Expand the combined box by 10%.
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

                    // We've consumed this key press. @Cleanup: Delete all these once per frame.
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
                        // @Speed: Store exp0 and exp1 on the animation object.
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

            // @Speed: We only want to re-buffer the vertices if they've changed.
            // This seems to be especially important for performance on Firefox.
            gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);

            gl.drawArrays(gl.TRIANGLES, 0, vertices.length/6); // 6 is the number of floats per vertex attribute.
        }

        // 2D canvas:
        {
            ui.scale(dpr, dpr);
            ui.clearRect(0, 0, windowWidth, windowHeight);

            // Draw the labels loaded from the JSON file.
            {
                const height = 16;
                ui.font = `${height}px sans serif`;
                ui.textBaseline = "top";

                const resolution = 256;
                const usedSpace  = new Int8Array(resolution);
                // x = num columns, y = num rows:
                //        x*y = resolution
                //        x/y = ratio
                //          x = y*ratio
                //  ratio*y^2 = resolution
                //          y = sqrt(resolution/ratio)
                const ratio   = windowWidth/windowHeight;
                const numRows = Math.floor(Math.sqrt(resolution/ratio));
                const numCols = Math.floor(numRows*ratio);

                // Draw the labels.
                for (const label of labels) {
                    const screenPos = xform(map.currentTransform, label.pos);

                    let offScreen = false; { // @Cleanup. Overlap function?
                        if (screenPos[0] < 0)                  offScreen = true;
                        else if (screenPos[0] > windowWidth)   offScreen = true;
                        else if (screenPos[1] < 0)             offScreen = true;
                        else if (screenPos[1] > windowHeight)  offScreen = true;
                    }
                    if (offScreen)  continue;

                    const {width} = ui.measureText(label.text);
                    const x = screenPos[0] - width/2;
                    const y = screenPos[1] - height/2;

                    // To stop labels from overlapping, look at the usedSpace array. These are bytes
                    // we turn from 0 to 1 when the corresponding pixel-space is used by a label.
                    {
                        // x1 and y1 are the bottom-right corner of the text box.
                        const x1 = x + width;
                        const y1 = y + height;

                        const colWidth  = windowWidth/numCols;
                        const rowHeight = windowHeight/numRows;
                        // Attempt to make the labels stable across translations:
                        const offsetX = map.currentTransform.translateX % colWidth;
                        const offsetY = map.currentTransform.translateY % rowHeight;

                        // Convert the text's bounding box points to their usedSpace indices.
                        const ix  = Math.floor(((x-offsetX)/windowWidth)*numCols);
                        const iy  = Math.floor(((y-offsetY)/windowHeight)*numRows);
                        const ix1 = Math.floor(((x1-offsetX)/windowWidth)*numCols);
                        const iy1 = Math.floor(((y1-offsetY)/windowHeight)*numRows);

                        let used = false;
                        {
                            let col = ix;
                            let row = iy;
                            while (row <= iy1) {
                                // Check if anyone else is using the space.
                                const index = numCols*row + col;
                                if (usedSpace[index]) { // @Todo: Could this index be outside the array's range? What would happen?
                                    used = true;
                                    break;
                                }
                                col += 1;
                                if (col <= ix1)  continue;
                                col = ix;
                                row += 1;
                            }
                        }

                        //if (used)  break;      // Stop trying to add labels once one of them doesn't fit.
                        if (used)  continue; // Keep trying to fit all the labels.

                        // We will now use the space.
                        {
                            let col = ix;
                            let row = iy;
                            while (row <= iy1) {
                                const index = numCols*row + col;
                                usedSpace[index] = 1;

                                col += 1;
                                if (col <= ix1)  continue;
                                col = ix;
                                row += 1;
                            }
                        }
                    }

                    ui.fillStyle = 'white';
                    ui.fillText(label.text, x, y);
                }

                if (debugLabels) { // Visualise the usedSpace grid. @Debug
                    ui.lineWidth = 1;
                    ui.strokeStyle = 'rgba(255,255,255,0.5)';

                    const colWidth  = windowWidth/numCols;
                    const rowHeight = windowHeight/numRows;

                    for (let i = 0; i < numCols+1; i++) {
                        ui.moveTo(colWidth*i, 0);
                        ui.lineTo(colWidth*i, windowHeight);
                    }
                    for (let i = 0; i < numRows+1; i++) {
                        ui.moveTo(0,           rowHeight*i);
                        ui.lineTo(windowWidth, rowHeight*i);
                    }
                    ui.stroke();

                    ui.fillStyle = 'rgba(255,255,255,0.35)';
                    for (let row = 0; row < numRows; row++) {
                        for (let col = 0; col < numCols; col++) {
                            const index = numCols*row + col;
                            if (usedSpace[index]) {
                                ui.fillRect(colWidth*col, rowHeight*row, colWidth, rowHeight);
                            }
                        }
                    }
                }
            }

            if (debugTransform) { // Draw the map's current transform in the bottom-right corner of the canvas. @Debug
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
