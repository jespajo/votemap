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

document.addEventListener("DOMContentLoaded", async () => {
	const canvas = document.querySelector("canvas");

	const gl = canvas.getContext("webgl") || canvas.getContext("experimental-webgl");
	if (!gl) {
		console.error("Failed to get WebGL context.");
		return;
	}

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
	}

	const u_proj = gl.getUniformLocation(program, "u_proj"); // Transforms pixel space to UV space. Only changes when the screen dimensions change.
	const u_view = gl.getUniformLocation(program, "u_view"); // Applies current pan/zoom. User-controlled.

	const view = new Float32Array([1,0,0, 0,1,0, 0,0,1]);

	{
		const buffer = gl.createBuffer();
		gl.bindBuffer(gl.ARRAY_BUFFER, buffer);

		const v_position = gl.getAttribLocation(program, "v_position");
		const v_colour   = gl.getAttribLocation(program, "v_colour");

		gl.enableVertexAttribArray(v_position);
		gl.enableVertexAttribArray(v_colour);

		gl.vertexAttribPointer(v_position, 2, gl.FLOAT, false, 6*4, 0);
		gl.vertexAttribPointer(v_colour,   4, gl.FLOAT, false, 6*4, 8); // @Cleanup: Avoid hard-coding these numbers.
	}

	let vertices; {
		const response = await fetch("../bin/vertices");
		const data     = await response.arrayBuffer();

		vertices = new Float32Array(data);
	}

	;(function step() {
		const width  = Math.floor(canvas.parentElement.clientWidth);
		const height = Math.floor(canvas.parentElement.clientHeight);

		const proj = new Float32Array([1,0,0, 0,1,0, 0,0,1]);
        {
			// Transform from pixel space to UV space. Flip the y-axis for top-left origin.
			proj[0] =  2/width;   // X scale.
			proj[4] = -2/height;  // Y scale.
			proj[6] = -1; 		  // X translate.
			proj[7] =  1; 		  // Y translate.
		}

		canvas.width  = width;
		canvas.height = height;

		gl.viewport(0, 0, width, height);

		gl.clearColor(0.0, 0.0, 0.0, 1.0);
		gl.clear(gl.COLOR_BUFFER_BIT);

		gl.useProgram(program);

	    gl.uniformMatrix3fv(u_proj, false, proj);
	    gl.uniformMatrix3fv(u_view, false, view);

		gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);

		gl.drawArrays(gl.TRIANGLES, 0, vertices.length/6); // 6 is the number of floats per vertex attribute.

		window.requestAnimationFrame(step);
	})();

	//
	// Mouse/touch events.
	//

	canvas.addEventListener("wheel", event => {
		const oldScale = view[0];
		const newScale = view[0]*((event.deltaY > 0) ? 0.75 : 1.5);

		const originX = (event.clientX - view[6])/oldScale;
		const originY = (event.clientY - view[7])/oldScale;

		view[0] = newScale;
		view[4] = newScale;

		view[6] = event.clientX - originX*newScale;
		view[7] = event.clientY - originY*newScale;
	}, {passive: true});

	let dragging = false;
	let mouseX = 0;
	let mouseY = 0;
	canvas.addEventListener("pointerdown", event => {
		dragging = true;
		mouseX = event.clientX;
		mouseY = event.clientY;
	});
	window.addEventListener("pointerup", event => {
		dragging = false;
	});
	window.addEventListener("pointermove", event => {
		if (!dragging)  return;

		view[6] += (event.clientX - mouseX);
		view[7] += (event.clientY - mouseY);

		mouseX = event.clientX;
		mouseY = event.clientY;
	});

	// For debugging.
	window.view = view;
	window.vertices = vertices;
});
