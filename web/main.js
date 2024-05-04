const VERTEX_SHADER_TEXT = `
	precision highp float;

	uniform mat3 u_matrix;

	attribute vec2 v_position;
	attribute vec4 v_colour;

	varying vec4 f_colour;

	void main()
	{
		vec3 position = u_matrix * vec3(v_position, 1.0);

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

	const u_matrix = gl.getUniformLocation(program, "u_matrix");

	let scale = 1;
	const translate = {x:0, y:0};

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

		const matrix = new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1]); // @Naming. ctm? mvp? xform?
        {
			// Make the matrix transform from pixel space to UV space. Flip the y-axis so we can
			// reference pixels from the top-left corner.
			matrix[0] =  2/width;   // X scale.
			matrix[4] = -2/height;  // Y scale.
			matrix[6] = -1; 		// X translate.
			matrix[7] =  1; 		// Y translate.

			// Apply the user-controlled transform.
			matrix[0] *= scale;
			matrix[4] *= scale;
			matrix[6] += translate.x;
			matrix[7] += translate.y;
        }


		canvas.width  = width;
		canvas.height = height;

		gl.viewport(0, 0, width, height);

		gl.clearColor(0.0, 0.0, 0.0, 1.0);
		gl.clear(gl.COLOR_BUFFER_BIT);

		gl.useProgram(program);

	    gl.uniformMatrix3fv(u_matrix, false, matrix);

		gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);

		gl.drawArrays(gl.TRIANGLES, 0, vertices.length/6); // 6 is the number of floats per vertex attribute.

		window.requestAnimationFrame(step);
	})();

	document.addEventListener("keydown", event => {
		switch (event.key) {
			case "ArrowUp":
				translate.y += 0.1;
				break;
			case "ArrowDown":
				translate.y -= 0.1;
				break;
			case "ArrowLeft":
				translate.x -= 0.1;
				break;
			case "ArrowRight":
				translate.x += 0.1;
				break;
			case "w":
				scale *= 1.1;
				break;
			case "s":
				scale /= 1.1;
				break;
			default:
				return;
		}
		event.preventDefault();
	});
});
