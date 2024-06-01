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

function xform(vec2, mat3) {
    // Called xform because it's simple. Although we pass this function a full 3x3 matrix (column-major order),
    // we only use three values from this matrix. The top-left value is taken as the scale, which is assumed
    // to be uniform for x and y. Then we apply the x and y translations. @Hack.
    const scale = mat3[0];
    const translate = [mat3[6], mat3[7]];

    const x = scale*vec2[0] + translate[0];
    const y = scale*vec2[1] + translate[1];

    return [x, y];
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
        const data     = await response.arrayBuffer();

        vertices = new Float32Array(data);
    }

    //
    // Handle user input.
    //

    const input = {
        // pointer[0] is the mouse or the first finger to touch the screen. pointer[1] is the second finger.
        pointers: [ // X and Y are in screen coordinates.
            {down: false, x: 0, y: 0},
            {down: false, x: 0, y: 0, id: 0},
        ], 

        // +1 for scroll down, -1 for scroll up. @Todo: Scroll sensitivity? Horizontal scroll?
        scroll: 0,
    };
    window.addEventListener("pointerdown", event => {
        const [ptr0, ptr1] = input.pointers;

        if (event.isPrimary) {
            // The user has pressed the mouse or touched the screen with one finger.
            ptr0.down = true;
            ptr0.x    = event.clientX;
            ptr0.y    = event.clientY;
        } else if (!ptr1.down) {
            // The user has touched the screen with a second finger. (If ptr1 had been down, this
            // event would be a third finger, which we ignore.)
            ptr1.id   = event.pointerId;
            ptr1.down = true;
            ptr1.x    = event.clientX;
            ptr1.y    = event.clientY;
        }
    });
    window.addEventListener("pointerup", event => {
        const [ptr0, ptr1] = input.pointers;

        if (event.isPrimary) {
            ptr0.down = false;
            ptr0.x    = event.clientX;
            ptr0.y    = event.clientY;
        } else if (ptr1.down && ptr1.id == event.pointerId) {
            ptr1.down = false;
            ptr1.x    = event.clientX;
            ptr1.y    = event.clientY;
        }
    });
    window.addEventListener("pointermove", event => {
        const [ptr0, ptr1] = input.pointers;

        if (event.isPrimary) {
            ptr0.x = event.clientX;
            ptr0.y = event.clientY;
        } else if (ptr1.down && ptr1.id == event.pointerId) {
            ptr1.x = event.clientX;
            ptr1.y = event.clientY;
        }
    });
    window.addEventListener("wheel", event => {
        input.scroll = (event.deltaY < 0) ? -1 : 1;
    }, {passive: true});

    // Map state variables:
    const map = {
        currentTransform: {
            scale:     1,
            rotate:    0, // The angle, in radians, of a counter-clockwise rotation.
            translate: {x: 0, y: 0},
        },

        // When the user presses their mouse down on the map, we lock the mouse position to its
        // current location on the map. On a touchscreen, the user can use up to two fingers.
        pointerLocks: [  // X and Y are in map coordinates.
            {locked: false, x: 0, y: 0},
            {locked: false, x: 0, y: 0},
        ],
    };

    //
    // The main loop (runs once per frame):
    //
    ;(function step() {
        //
        // Update state.
        //

        const width  = Math.floor(document.body.clientWidth);
        const height = Math.floor(document.body.clientHeight);

        // Handle user events on the map.
        {
            const [ptr0, ptr1]   = input.pointers;
            const [lock0, lock1] = map.pointerLocks;
            const ct             = map.currentTransform;

            if (ptr0.down) {
                if (!lock0.locked) {
                    lock0.locked = true;

                    lock0.x = (ptr0.x - ct.translate.x)/ct.scale;
                    lock0.y = (ptr0.y - ct.translate.y)/ct.scale;
                }
            } else {
                lock0.locked = false;
            }

            if (lock0.locked) {
                ct.translate.x += ptr0.x - (ct.scale*lock0.x + ct.translate.x);
                ct.translate.y += ptr0.y - (ct.scale*lock0.y + ct.translate.y);
            }


            if (input.scroll) {
                const originX = (ptr0.x - ct.translate.x)/ct.scale;
                const originY = (ptr0.y - ct.translate.y)/ct.scale;

                ct.scale *= (input.scroll < 0) ? 1.5 : 0.75;

                ct.translate.x = ptr0.x - ct.scale*originX;
                ct.translate.y = ptr0.y - ct.scale*originY;

                // We've consumed the scroll!
                input.scroll = 0;
            } 
        }

        const proj = new Float32Array([1,0,0, 0,1,0, 0,0,1]);
        {
            // Transform from pixel space to UV space. Flip the y-axis for top-left origin.
            proj[0] =  2/width;     // X scale.
            proj[4] = -2/height;    // Y scale.
            proj[6] = -1;           // X translate.
            proj[7] =  1;           // Y translate.
        }

        const view = new Float32Array([1,0,0, 0,1,0, 0,0,1]);
        {
            const {scale, rotate, translate} = map.currentTransform;

            view[0] = scale;
            view[4] = scale;
            view[6] = translate.x;
            view[7] = translate.y;
        }

        //
        // Draw the page.
        //

        $$("canvas").forEach(canvas => {
            canvas.width  = width;
            canvas.height = height;
        });

        // WebGL canvas:
        {
            gl.viewport(0, 0, width, height);

            gl.clearColor(0.75, 0.75, 0.75, 1.0); // Background colour (same as water): light grey.
            gl.clear(gl.COLOR_BUFFER_BIT);

            gl.useProgram(program);

            gl.uniformMatrix3fv(u_proj, false, proj);
            gl.uniformMatrix3fv(u_view, false, view);

            gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);

            gl.drawArrays(gl.TRIANGLES, 0, vertices.length/6); // 6 is the number of floats per vertex attribute.
        }

        // 2D canvas:
        {
            ui.clearRect(0, 0, width, height);

            // Draw a square in the centre of Australia.
            {
                const austCentre = xform([254405, 2772229], view);

                const size = 10;
                ui.fillStyle = 'red';
                ui.fillRect(austCentre[0]-size/2, austCentre[1]-size/2, size, size);
            }

            // Draw a border.
            {
                const size = 20;
                ui.fillStyle = '#141414';
                ui.fillRect(0, 0, width, size);
                ui.fillRect(0, 0, size, height);
                ui.fillRect(width-size, 0, size, height);
                ui.fillRect(0, height-size, width, size);
            }
        }

        window.requestAnimationFrame(step);
    })();

    // When the page loads, fit Australia on the screen.
    {
        const austBox = {x1: -1863361, y1: 1168642, x2: 2087981, y2: 4840595};

        const austWidth  = austBox.x2 - austBox.x1;
        const austHeight = austBox.y2 - austBox.y1;

        const screenWidth  = document.body.clientWidth;
        const screenHeight = document.body.clientHeight;

        const austRatio   = austWidth/austHeight;
        const screenRatio = screenWidth/screenHeight;

        const ct = map.currentTransform;

        if (austRatio < screenRatio) {
            // The screen is wider than Australia.
            ct.scale = 0.9*(screenHeight/austHeight);
        } else {
            // The screen is taller than Australia.
            ct.scale = 0.9*(screenWidth/austWidth);  
        }

        ct.translate.x = -ct.scale*austBox.x1 + (screenWidth - ct.scale*austWidth)/2;
        ct.translate.y = -ct.scale*austBox.y1 + (screenHeight - ct.scale*austHeight)/2;
    }
});
