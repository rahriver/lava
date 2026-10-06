// lava — Layer Audio VisuAlizer: an audio visualizer drawn on the desktop (wlr-layer-shell + OpenGL ES 2, spectrum from cava).
// SIGUSR2 reloads the colors (pywal's ~/.cache/wal/colors.json, built-in palette otherwise).

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

enum { BARS, MOUNTAINS, MIRROR, TOP, CIRCLE, WAVE };

struct style {
	const char *name;
	uint32_t anchor;     // 0 = centered
	float size;          // height, or diameter for the circle, as a fraction of the screen height
	int pitch, count;    // px per bar, or a fixed bar count when pitch is 0
};

#define A_BOT (ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)
#define A_TOP (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)

static const struct style STYLES[] = {
	[BARS]      = { "bars",      A_BOT, 0.29, 24, 0 },
	[MOUNTAINS] = { "mountains", A_BOT, 0.29, 20, 0 },
	[MIRROR]    = { "mirror",    A_BOT, 0.39, 24, 0 },
	[TOP]       = { "top",       A_TOP, 0.26, 24, 0 },
	[CIRCLE]    = { "circle",    0,     0.69, 0,  120 },
	[WAVE]      = { "wave",      A_BOT, 0.29, 0,  120 },
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_output *output;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_egl_window *egl_window;
static EGLDisplay egl_dpy;
static EGLSurface egl_surf;

static int style, width, height, configured, frame_pending, dirty;
static int fps = 60, size_px, screen_h, screen_scale = 1;
static volatile sig_atomic_t running = 1, reload_colors = 1;

static int nbars;
static uint16_t *vals;
static unsigned char *texbuf;
static pid_t cava_pid;
static int cava_fd = -1;
static GLuint prog, tex;
static float colors[5][3];

static void die(const char *msg) { fprintf(stderr, "lava: %s\n", msg); exit(1); }

static void load_colors(void)
{
	static const char *keys[5] = { "color4", "color3", "color5", "color2", "color6" };
	static const unsigned fallback[5] = { 0x60A4EF, 0x57A7E3, 0xA0A5F2, 0x9375A3, 0xD2A7E0 };
	char path[512], buf[8192] = { 0 };
	snprintf(path, sizeof path, "%s/.cache/wal/colors.json", getenv("HOME"));
	FILE *f = fopen(path, "r");
	if (f) { buf[fread(buf, 1, sizeof buf - 1, f)] = 0; fclose(f); }
	for (int i = 0; i < 5; i++) {
		unsigned c = fallback[i];
		char needle[32];
		snprintf(needle, sizeof needle, "\"%s\"", keys[i]);
		char *p = strstr(buf, needle);
		if (p && (p = strchr(p + strlen(needle), '#')))
			c = (unsigned)strtoul(p + 1, NULL, 16);
		colors[i][0] = ((c >> 16) & 255) / 255.f;
		colors[i][1] = ((c >> 8) & 255) / 255.f;
		colors[i][2] = (c & 255) / 255.f;
	}
}

static void start_cava(void)
{
	const struct style *s = &STYLES[style];
	nbars = s->pitch ? width / s->pitch : s->count;
	nbars = (nbars / 2) * 2;                       // stereo needs an even count
	if (nbars < 16) nbars = 16;
	if (nbars > 400) nbars = 400;
	vals = calloc(nbars, sizeof *vals);
	texbuf = calloc(nbars, 2);

	char conf[512];
	const char *rt = getenv("XDG_RUNTIME_DIR");
	snprintf(conf, sizeof conf, "%s/lava-cava.conf", rt ? rt : "/tmp");
	FILE *f = fopen(conf, "w");
	if (!f) die("can't write cava config");
	fprintf(f,
		"[general]\nframerate = %d\nautosens = 1\nbars = %d\nsleep_timer = 5\n"
		"[input]\nmethod = pipewire\nsource = auto\n"
		"[output]\nmethod = raw\nraw_target = /dev/stdout\ndata_format = binary\nbit_format = 16bit\n"
		"channels = stereo\nmono_option = average\n"
		"[smoothing]\nnoise_reduction = 77\n", fps, nbars);
	fclose(f);

	int pfd[2];
	if (pipe2(pfd, O_CLOEXEC) < 0) die("pipe");
	cava_pid = fork();
	if (cava_pid < 0) die("fork");
	if (cava_pid == 0) {
		prctl(PR_SET_PDEATHSIG, SIGTERM);          // cava dies with us
		dup2(pfd[1], 1);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0) dup2(null, 2);
		execlp("cava", "cava", "-p", conf, (char *)NULL);
		_exit(127);
	}
	close(pfd[1]);
	cava_fd = pfd[0];
	fcntl(cava_fd, F_SETFL, O_NONBLOCK);
}

// keep only the newest complete frame so the picture never lags behind the audio
static int read_cava(void)
{
	static unsigned char buf[65536];
	static size_t have;
	size_t frame = (size_t)nbars * 2;
	int got = 0;
	for (;;) {
		ssize_t r = read(cava_fd, buf + have, sizeof buf - have);
		if (r == 0) return -1;
		if (r < 0) {
			if (errno == EAGAIN || errno == EINTR) break;
			return -1;
		}
		have += r;
		size_t frames = have / frame;
		if (frames) {
			memcpy(vals, buf + (frames - 1) * frame, frame);
			memmove(buf, buf + frames * frame, have - frames * frame);
			have -= frames * frame;
			got = 1;
		}
	}
	return got;
}

static const char *VERT =
	"attribute vec2 p;\n"
	"void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";

static const char *FRAG =
	"precision highp float;\n"
	"uniform sampler2D vals;\n"
	"uniform float n;\n"
	"uniform vec2 res;\n"
	"uniform int style;\n"
	"uniform vec3 c0, c1, c2, c3, c4;\n"
	"const float PI = 3.14159265;\n"
	// bar height packed as 16 bits in a LUMINANCE_ALPHA texel
	"float val(float i) {\n"
	"  vec4 t = texture2D(vals, vec2((clamp(i, 0.0, n - 1.0) + 0.5) / n, 0.5));\n"
	"  return (t.r * 255.0 * 256.0 + t.a * 255.0) / 65535.0;\n"
	"}\n"
	// Catmull-Rom curve through the bar heights
	"float sval(float x) {\n"
	"  float i = floor(x), f = x - i;\n"
	"  float p0 = val(i - 1.0), p1 = val(i), p2 = val(i + 1.0), p3 = val(i + 2.0);\n"
	"  float v = p1 + 0.5 * f * (p2 - p0 + f * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + f * (3.0 * (p1 - p2) + p3 - p0)));\n"
	"  return clamp(v, 0.0, 1.0);\n"
	"}\n"
	"vec3 grad(float t) {\n"
	"  t = clamp(t, 0.0, 1.0) * 4.0;\n"
	"  vec3 c = mix(c0, c1, clamp(t, 0.0, 1.0));\n"
	"  c = mix(c, c2, clamp(t - 1.0, 0.0, 1.0));\n"
	"  c = mix(c, c3, clamp(t - 2.0, 0.0, 1.0));\n"
	"  return mix(c, c4, clamp(t - 3.0, 0.0, 1.0));\n"
	"}\n"
	// antialiased rounded bar growing from y = 0
	"float bar(vec2 q, float pitch, float hmax) {\n"
	"  float i = floor(q.x / pitch);\n"
	"  float v = val(i);\n"
	"  float r = pitch * 0.35;\n"
	"  float cx = (i + 0.5) * pitch;\n"
	"  float top = max(v * hmax - r, r);\n"
	"  float d = length(vec2(q.x - cx, q.y - clamp(q.y, r, top))) - r;\n"
	"  return clamp(0.5 - d, 0.0, 1.0) * step(0.006, v);\n"
	"}\n"
	"void main() {\n"
	"  vec2 q = gl_FragCoord.xy;\n"
	"  vec3 col = vec3(0.0); float a = 0.0;\n"
	"  if (style == 0 || style == 3) {\n"   // bars / top
	"    if (style == 3) q.y = res.y - q.y;\n"
	"    a = bar(q, res.x / n, res.y);\n"
	"    col = grad(q.y / res.y);\n"
	"  } else if (style == 2) {\n"   // mirror
	"    q.y = abs(q.y - res.y * 0.5);\n"
	"    a = bar(q, res.x / n, res.y * 0.5);\n"
	"    col = grad(q.y / (res.y * 0.5));\n"
	"  } else if (style == 1) {\n"   // mountains
	"    float h = sval(q.x / (res.x / n) - 0.5) * res.y;\n"
	"    float fill = clamp(h - q.y + 0.5, 0.0, 1.0) * (0.25 + 0.4 * q.y / max(h, 1.0));\n"
	"    float line = clamp(1.8 - abs(q.y - h), 0.0, 1.0) * step(1.0, h);\n"
	"    a = max(fill, line);\n"
	"    col = grad(q.y / res.y);\n"
	"  } else if (style == 5) {\n"   // wave
	"    float mid = res.y * 0.5;\n"
	"    float h = sval(q.x / res.x * (n - 1.0)) * (mid - 4.0);\n"
	"    float d = abs(q.y - mid);\n"
	"    float fill = clamp(h - d + 0.5, 0.0, 1.0) * 0.28;\n"
	"    float line = clamp(1.6 - abs(d - h), 0.0, 1.0);\n"
	"    a = max(fill, line);\n"
	"    col = grad(q.x / res.x);\n"
	"  } else {\n"   // circle
	"    vec2 p = q - res * 0.5;\n"
	"    float rmax = min(res.x, res.y) * 0.5 - 8.0;\n"
	"    float r0 = rmax * 0.45, span = rmax * 0.55;\n"
	"    float bw = 2.0 * PI * r0 / n * 0.55;\n"
	"    float ang = atan(p.x, -p.y);\n"                          // 0 at the bottom, clockwise
	"    if (ang < 0.0) ang += 2.0 * PI;\n"
	"    float i = floor(ang / (2.0 * PI) * n);\n"
	"    float ca = (i + 0.5) / n * 2.0 * PI;\n"
	"    vec2 dir = vec2(sin(ca), -cos(ca));\n"
	"    float along = dot(p, dir), perp = dot(p, vec2(dir.y, -dir.x));\n"
	"    float len = max(val(i) * span, 0.5);\n"
	"    float d = length(vec2(perp, along - clamp(along, r0, r0 + len))) - bw * 0.5;\n"
	"    float ab = clamp(0.5 - d, 0.0, 1.0);\n"
	"    float ring = clamp(1.0 - abs(length(p) - (r0 - bw * 1.2)), 0.0, 1.0) * 0.35;\n"
	"    a = max(ab, ring);\n"
	"    col = ab >= ring ? grad((along - r0) / span) : c0;\n"
	"  }\n"
	"  a *= 0.95;\n"
	"  gl_FragColor = vec4(col * a, a);\n"                          // premultiplied alpha
	"}\n";

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		fprintf(stderr, "%s\n", log);
		die("shader compile failed");
	}
	return s;
}

static void gl_init(void)
{
	prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, VERT));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, FRAG));
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) die("shader link failed");
	glUseProgram(prog);

	static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, nbars, 1, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, texbuf);

	glUniform1i(glGetUniformLocation(prog, "vals"), 0);
	glUniform1f(glGetUniformLocation(prog, "n"), (float)nbars);
	glUniform1i(glGetUniformLocation(prog, "style"), style);
	glDisable(GL_BLEND);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	(void)data; (void)time;
	wl_callback_destroy(cb);
	frame_pending = 0;
}
static const struct wl_callback_listener frame_listener = { frame_done };

static void render(void)
{
	if (reload_colors) {
		reload_colors = 0;
		load_colors();
		static const char *names[5] = { "c0", "c1", "c2", "c3", "c4" };
		for (int i = 0; i < 5; i++)
			glUniform3fv(glGetUniformLocation(prog, names[i]), 1, colors[i]);
	}
	for (int i = 0; i < nbars; i++) {
		texbuf[i * 2] = vals[i] >> 8;
		texbuf[i * 2 + 1] = vals[i] & 255;
	}
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, nbars, 1, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, texbuf);
	glViewport(0, 0, width, height);
	glUniform2f(glGetUniformLocation(prog, "res"), (float)width, (float)height);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	struct wl_callback *cb = wl_surface_frame(surface);   // no callbacks while hidden, so no drawing
	wl_callback_add_listener(cb, &frame_listener, NULL);
	frame_pending = 1;
	dirty = 0;
	eglSwapBuffers(egl_dpy, egl_surf);
}

static void egl_init(void)
{
	egl_dpy = eglGetDisplay((EGLNativeDisplayType)display);
	if (!egl_dpy || !eglInitialize(egl_dpy, NULL, NULL)) die("EGL init failed");
	eglBindAPI(EGL_OPENGL_ES_API);
	static const EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
	EGLConfig cfg;
	EGLint n;
	if (!eglChooseConfig(egl_dpy, cfg_attr, &cfg, 1, &n) || n < 1) die("no EGL config");
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (!ctx) die("no EGL context");
	egl_window = wl_egl_window_create(surface, width, height);
	egl_surf = eglCreateWindowSurface(egl_dpy, cfg, (EGLNativeWindowType)egl_window, NULL);
	if (egl_surf == EGL_NO_SURFACE) die("no EGL surface");
	eglMakeCurrent(egl_dpy, egl_surf, egl_surf, ctx);
	eglSwapInterval(egl_dpy, 0);                          // frame callbacks do the pacing
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h)
{
	(void)data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if ((int)w != width || (int)h != height) {
		width = w; height = h;
		if (egl_window) wl_egl_window_resize(egl_window, width, height, 0, 0);
		dirty = 1;
	}
	configured = 1;
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) { (void)data; (void)ls; running = 0; }
static const struct zwlr_layer_surface_v1_listener layer_listener = { layer_configure, layer_closed };

static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
	(void)data; (void)o; (void)w; (void)refresh;
	if (flags & WL_OUTPUT_MODE_CURRENT) screen_h = h;
}
static void output_scale(void *data, struct wl_output *o, int32_t factor) { (void)data; (void)o; screen_scale = factor; }
static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp,
	const char *make, const char *model, int32_t t) { (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sp; (void)make; (void)model; (void)t; }
static void output_done(void *data, struct wl_output *o) { (void)data; (void)o; }
static const struct wl_output_listener output_listener = {
	.geometry = output_geometry, .mode = output_mode, .done = output_done, .scale = output_scale };

static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
	(void)data; (void)ver;
	if (!strcmp(iface, wl_compositor_interface.name))
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_output_interface.name) && !output) {
		output = wl_registry_bind(reg, name, &wl_output_interface, ver < 2 ? ver : 2);
		wl_output_add_listener(output, &output_listener, NULL);
	}
	else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
		layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
}
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) { (void)data; (void)reg; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void on_signal(int sig)
{
	if (sig == SIGUSR2) reload_colors = 1, dirty = 1;
	else running = 0;
}

static void usage(int code)
{
	fprintf(code ? stderr : stdout,
		"usage: lava [-s px] [-f fps] [bars|mountains|mirror|top|circle|wave]\n"
		"  -s px   height (circle: diameter) in pixels; default scales with the screen\n"
		"  -f fps  frame rate, 10-240 (default 60)\n");
	exit(code);
}

int main(int argc, char **argv)
{
	int opt;
	while ((opt = getopt(argc, argv, "s:f:h")) != -1) {
		if (opt == 's') size_px = atoi(optarg);
		else if (opt == 'f') fps = atoi(optarg);
		else usage(opt != 'h');
	}
	if (fps < 10 || fps > 240) usage(2);
	style = BARS;
	if (optind < argc) {
		style = -1;
		for (int i = 0; i < (int)(sizeof STYLES / sizeof *STYLES); i++)
			if (!strcmp(argv[optind], STYLES[i].name)) style = i;
		if (style < 0) usage(2);
	}
	struct sigaction sa = { .sa_handler = on_signal };     // no SA_RESTART, so poll() wakes up
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	display = wl_display_connect(NULL);
	if (!display) die("can't connect to the Wayland display");
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !layer_shell) die("compositor lacks wlr-layer-shell");
	wl_display_roundtrip(display);   // output mode/scale events

	const struct style *s = &STYLES[style];
	surface = wl_compositor_create_surface(compositor);
	struct wl_region *empty = wl_compositor_create_region(compositor);
	wl_surface_set_input_region(surface, empty);           // click-through
	wl_region_destroy(empty);
	int px = size_px > 0 ? size_px : (int)(s->size * (screen_h ? screen_h / screen_scale : 1080));
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "lava");   // above the wallpaper, below windows
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
	zwlr_layer_surface_v1_set_size(layer_surface, s->anchor ? 0 : px, px);
	zwlr_layer_surface_v1_set_anchor(layer_surface, s->anchor);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, 0);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 0);
	wl_surface_commit(surface);
	while (!configured && wl_display_dispatch(display) != -1)
		;
	if (!width || !height) die("compositor gave no size");

	start_cava();
	egl_init();
	gl_init();
	dirty = 1;

	struct pollfd fds[2] = { { wl_display_get_fd(display), POLLIN, 0 }, { cava_fd, POLLIN, 0 } };
	while (running) {
		while (wl_display_prepare_read(display) != 0)
			wl_display_dispatch_pending(display);
		wl_display_flush(display);
		int r = poll(fds, 2, -1);
		if (r < 0) {
			wl_display_cancel_read(display);
			if (errno == EINTR) goto draw;
			break;
		}
		if (fds[0].revents & POLLIN) {
			if (wl_display_read_events(display) < 0) break;
		} else {
			wl_display_cancel_read(display);
		}
		if (fds[0].revents & (POLLERR | POLLHUP)) break;
		wl_display_dispatch_pending(display);
		if (fds[1].revents & (POLLIN | POLLHUP)) {
			int got = read_cava();
			if (got < 0) break;
			if (got) dirty = 1;
		}
	draw:
		if (dirty && !frame_pending && running)
			render();
	}

	if (cava_pid > 0) { kill(cava_pid, SIGTERM); waitpid(cava_pid, NULL, 0); }
	return 0;
}
