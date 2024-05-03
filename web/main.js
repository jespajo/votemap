const VERTEX_SHADER_TEXT = `
	precision highp float;

	uniform vec2 u_scale;
	uniform vec2 u_translate;

	attribute vec2 v_position;
	attribute vec4 v_colour;

	varying vec4 f_colour;

	void main()
	{
		vec2 position = u_scale * v_position + u_translate;

		gl_Position = vec4(position, 0.0, 1.0);

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
		console.log("Failed to get WebGL context.");
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

	const scale     = new Float32Array([0.00001, 0.00001]);
	const translate = new Float32Array([6, 15]);

	const u_scale     = gl.getUniformLocation(program, "u_scale");
	const u_translate = gl.getUniformLocation(program, "u_translate");

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

		canvas.width  = width;
		canvas.height = height;

		gl.viewport(0, 0, width, height);

		gl.clearColor(0.0, 0.0, 0.0, 1.0);
		gl.clear(gl.COLOR_BUFFER_BIT);

		gl.useProgram(program);

	    gl.uniform2fv(u_scale, scale);
	    gl.uniform2fv(u_translate, translate);

		gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);

		gl.drawArrays(gl.TRIANGLES, 0, vertices.length/6); // 6 is the number of floats per vertex attribute.

		window.requestAnimationFrame(step);
	})();

	document.addEventListener("keydown", event => {
		switch (event.key) {
			case "ArrowUp":
				translate[1] += 0.1;
				break;
			case "ArrowDown":
				translate[1] -= 0.1;
				break;
			case "ArrowLeft":
				translate[0] -= 0.1;
				break;
			case "ArrowRight":
				translate[0] += 0.1;
				break;
			case "w":
				scale[0] *= 1.1;
				scale[1] *= 1.1;
				break;
			case "s":
				scale[0] /= 1.1;
				scale[1] /= 1.1;
				break;
		}
	});
});
