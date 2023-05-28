#include <iostream>
#include <vector>
#include <algorithm>
#include <utility>
#include <tuple>
#include <random>
#include <optional>
#include <string>
#include <map>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <omp.h>

#include <gif-h/gif.h>

#include <glm/glm.hpp>
#include <glm/gtx/closest_point.hpp>
#include <glm/gtx/rotate_vector.hpp>

constexpr int WINDOW_WIDTH = 500;
constexpr int WINDOW_HEIGHT = 500;

GLFWwindow *glfw_init();

struct SingleChannel {
        using type = float;
        static constexpr int gl = GL_RED;
        static constexpr int channels = 1;
        static constexpr float clear = 0.0f;
};

struct ColorChannel {
        using type = glm::vec3;
        static constexpr int gl = GL_RGB;
        static constexpr int channels = 3;
        static constexpr glm::vec3 clear = glm::vec3 { 0.0f };
};

// Fixed channel image
template <typename Channel>
struct Image {
        int width;
        int height;
        typename Channel::type *data;

        static constexpr int gl = Channel::gl;
        static constexpr int channels = Channel::channels;

        Image(int w, int h) : width(w), height(h) {
                data = new typename Channel::type[width * height * Channel::channels] { Channel::clear };
        }

        // Move only
        Image(const Image &) = delete;
        Image(Image &&other) : width(other.width), height(other.height), data(other.data) {
                other.data = nullptr;
        }

        Image &operator=(const Image &) = delete;
        Image &operator=(Image &&other) {
                width = other.width;
                height = other.height;
                data = other.data;
                other.data = nullptr;
                return *this;
        }

        ~Image() {
                delete[] data;
        }

        typename Channel::type &operator()(int x, int y) {
                return data[y * width + x];
        }
        
        const typename Channel::type &operator()(int x, int y) const {
                return data[y * width + x];
        }

        typename Channel::type *operator[](int y) {
                return data + y * width;
        }
        
        const typename Channel::type *operator[](int y) const {
                return data + y * width;
        }

        void copy(const Image &other) {
                // If same dimensions, plain copy, otherwise use UV sampling
                if (width == other.width && height == other.height) {
                        std::copy(other.data, other.data + width * height * Channel::channels, data);
                } else {
                        #pragma omp parallel for
                        for (int y = 0; y < height; y++) {
                                for (int x = 0; x < width; x++) {
                                        float u = float(x) / width;
                                        float v = float(y) / height;

                                        int ox = u * other.width;
                                        int oy = v * other.height;

                                        data[y * width + x] = other.data[oy * other.width + ox];
                                }
                        }
                }
        }

        void clear(typename Channel::type val = typename Channel::type { 0 }) {
                for (int i = 0; i < width * height * Channel::channels; i++)
                        data[i] = val;
        }

        void smooth(int K) {
                Image <Channel> tmp(width, height);
                #pragma omp parallel for
                for (int y = 0; y < height; y++) {
                        for (int x = 0; x < width; x++) {
                                typename Channel::type sum = typename Channel::type { 0 };
                                for (int dy = -K; dy <= K; dy++) {
                                        for (int dx = -K; dx <= K; dx++) {
                                                int nx = x + dx;
                                                int ny = y + dy;
                                                if (nx < 0 || nx >= width || ny < 0 || ny >= height)
                                                        continue;
                                                sum += (*this)(nx, ny);
                                        }
                                }
                                tmp(x, y) = sum / ((2 * K + 1) * (2 * K + 1));
                        }
                }
                copy(tmp);
        }
};

// Render viewport configuration
struct {
        float aspect = float(WINDOW_WIDTH) / WINDOW_HEIGHT;
        // TODO: ranges as vec2
        float yrange = 1.0f; // Y from -1 to 1
        float xrange = yrange * aspect;
        float epsilon = std::min(xrange, yrange)/500.0f;

        // Cartesian to uv coordinates
        glm::vec2 to_uv(const glm::vec2 &point) {
                return (point + glm::vec2 { xrange, yrange }) / (2.0f * glm::vec2 { xrange, yrange });
        }

        glm::vec2 to_screen(const glm::vec2 &point) {
                return to_uv(point) * glm::vec2 { WINDOW_WIDTH, WINDOW_HEIGHT };
        }

        glm::vec2 to_cartesian(const glm::vec2 &uv) {
                return 2.0f * glm::vec2 { xrange, yrange } * uv - glm::vec2 { xrange, yrange };
        }

        float to_uv_x(float x) {
                return (x + xrange) / (2.0f * xrange);
        }

        float to_cartesian_y(float y) {
                return 2 * y * yrange - yrange;
        }
} viewport;

// Rasterizing lines
struct LineSegment {
        glm::vec2 v0;
        glm::vec2 v1;
};

// Closest distance from a point to a line segment
float distance(const LineSegment &line, const glm::vec2 &point)
{
        return glm::length(glm::closestPointOnLine(point, line.v0, line.v1) - point);
}

// TODO: thickness
void rasterize_line(Image <SingleChannel> &img, const LineSegment &line)
{
        float thickness = 2.0f * viewport.epsilon;
        glm::vec2 line_min = viewport.to_uv(glm::min(line.v0, line.v1) - thickness);
        glm::vec2 line_max = viewport.to_uv(glm::max(line.v0, line.v1) + thickness);

        int min_x = std::max(0, int(line_min.x * img.width));
        int min_y = std::max(0, int(line_min.y * img.height));
        int max_x = std::min(img.width, int(line_max.x * img.width));
        int max_y = std::min(img.height, int(line_max.y * img.height));

        // TODO: line search from min y to max y (including thickness)
        #pragma omp parallel for
        for (int y = min_y; y <= max_y; y++) {
                for (int x = min_x; x <= max_x; x++) {
                        if (x < 0 || x >= img.width || y < 0 || y >= img.height)
                                continue;

                        // Project point
                        glm::vec2 uv = glm::vec2 { x, y } / glm::vec2 { img.width, img.height };
                        glm::vec2 point = glm::vec2 { viewport.xrange, viewport.yrange } * (uv - 0.5f) * 2.0f;

                        float dist = distance(line, point);
                        if (dist < thickness) {
                                // Fall off after epsilon
                                float c = 1.0f - glm::smoothstep(0.0f, thickness, dist);
                                img(x, y) = std::max(img(x, y), c);
                        }
                }
        }
}

void rasterize_arrow(Image <SingleChannel> &img, const glm::vec2 &origin, const glm::vec2 &direction)
{
        glm::vec2 v0 = origin;
        glm::vec2 v1 = origin + direction;

        glm::vec2 left = -glm::rotate(direction, glm::radians(30.0f))/10.0f;
        glm::vec2 right = -glm::rotate(direction, glm::radians(-30.0f))/10.0f;

        rasterize_line(img, { v0, v1 });
        rasterize_line(img, { v1, v1 + left });
        rasterize_line(img, { v1, v1 + right });
}

void quiver(Image <SingleChannel> &img, const std::vector <glm::vec2> &points, const std::vector <glm::vec2> &directions)
{
        for (int i = 0; i < points.size(); i++)
                rasterize_arrow(img, points[i], directions[i]);
}

using Curve = std::vector <glm::vec2>;

void rasterize_boundary(Image <SingleChannel> &img, const Curve &curve)
{
        for (int i = 0; i < curve.size(); i++) {
                int j = (i + 1) % curve.size();
                rasterize_line(img, { curve[i], curve[j] });
        }
}

std::pair <glm::vec2, glm::vec2> bounds(const Curve &curve)
{
        glm::vec2 min { std::numeric_limits <float>::max() };
        glm::vec2 max { std::numeric_limits <float>::min() };

        for (const auto &point : curve) {
                min = glm::min(min, point);
                max = glm::max(max, point);
        }

        return { min, max };
}

// Rasterize interior of boundary from MIN to MAX (both Cartesian coordinates)
void rasterize_interior(Image <SingleChannel> &dst, const Curve &curve, const glm::vec2 &min, const glm::vec2 &max)
{
        // Use analytical horizontal line scan
        float thickness = 2.0f * viewport.epsilon;
        glm::vec2 min_uv = viewport.to_uv(min - thickness);
        glm::vec2 max_uv = viewport.to_uv(max + thickness);

        int min_x = std::max(0, int(min_uv.x * dst.width));
        int min_y = std::max(0, int(min_uv.y * dst.height));
        int max_x = std::min(dst.width, int(max_uv.x * dst.width));
        int max_y = std::min(dst.height, int(max_uv.y * dst.height));

        #pragma omp parallel for
        for (int y = min_y; y <= max_y; y++) {
                // Cartesian vertical component
                float cy = viewport.to_cartesian_y(float(y)/dst.height);

                // Check whether (and when) the curve intersects the horizontal line
                std::vector <float> intersections;
                for (int i = 0; i < curve.size(); i++) {
                        int j = (i + 1) % curve.size();
                        const glm::vec2 &p0 = curve[i];
                        const glm::vec2 &p1 = curve[j];

                        // Skip if the line is mostly horizontal
                        if (p0.y == p1.y)
                                continue;

                        // Skip if the line does not intersect the horizontal line
                        if (p0.y < cy && p1.y < cy)
                                continue;
                        if (p0.y > cy && p1.y > cy)
                                continue;

                        // Find the intersection
                        float t = (cy - p0.y) / (p1.y - p0.y);
                        float cx = p0.x + t * (p1.x - p0.x);

                        // Skip if the intersection is outside the viewport
                        if (cx < min.x - viewport.epsilon || cx > max.x + viewport.epsilon)
                                continue;

                        // Convert to screen coordinates
                        // TODO: smooth interpolation?
                        intersections.push_back(viewport.to_uv_x(cx));
                }

                // Sort, remove duplicates
                std::sort(intersections.begin(), intersections.end());
                intersections.erase(std::unique(intersections.begin(), intersections.end()), intersections.end());

                for (int i = 0; i < intersections.size(); i += 2) {
                        if (i + 1 >= intersections.size())
                                break;

                        float cx0 = intersections[i];
                        float cx1 = intersections[i + 1];

                        int x0 = std::max(min_x, int(cx0 * dst.width));
                        int x1 = std::min(max_x, int(cx1 * dst.width));

                        for (int x = x0; x <= x1; x++)
                                dst(x, y) = 1.0f;
                }
        }
}

// Intersection over union
float iou(const Image <SingleChannel> &a, const Image <SingleChannel> &b)
{
        int width = std::min(a.width, b.width);
        int height = std::min(a.height, b.height);

        float intersection = 0.0f;
        float union_ = 0.0f;

        for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                        float u = x / float(width);
                        float v = y / float(height);

                        int ax = int(u * a.width);
                        int ay = int(v * a.height);

                        int bx = int(u * b.width);
                        int by = int(v * b.height);

                        intersection += a(ax, ay) * b(bx, by);
                        union_ += std::max(a(ax, ay), b(bx, by));
                }
        }

        intersection /= width * height;
        union_ /= width * height;

        return intersection / union_;
}

// Compute gradient of line segment using interiors and point sample
std::pair <glm::vec2, glm::vec2> grad_segment(const Image <SingleChannel> &target, const Image <SingleChannel> &interior, const LineSegment &line, float t)
{
        assert(0 <= t && t <= 1.0f);

        // Normal (perpendicular) vector
        glm::vec2 d = glm::normalize(line.v1 - line.v0);
        glm::vec2 n = glm::normalize(glm::vec2(-d.y, d.x));
        glm::vec2 p = line.v0 + t * (line.v1 - line.v0);
        glm::vec2 dp { 0.0f };

        // Get derivative on neighborhood
        static constexpr int N = 3;

        // glm::vec2 screen_p = viewport.to_screen(p);
        glm::vec2 screen_uv = (p + glm::vec2 { viewport.xrange, viewport.yrange }) / (2.0f * glm::vec2 { viewport.xrange, viewport.yrange });
        glm::vec2 screen_p = screen_uv * glm::vec2 { target.width, target.height };

        float w_sum = 0.0f;
        for (int y = -N; y <= N; y++) {
                for (int x = -N; x <= N; x++) {
                        // Ensure valid image coordinates
                        if (screen_p.x + x < 0 || screen_p.x + x >= target.width)
                                continue;

                        if (screen_p.y + y < 0 || screen_p.y + y >= target.height)
                                continue;

                        // Compute weight
                        glm::vec2 q = screen_p + glm::vec2(x, y);
                        glm::vec2 uv = q / glm::vec2 { target.width, target.height };
                        glm::vec2 pt = viewport.to_cartesian(uv);

                        // Skip if on the line
                        if (std::fabs(glm::dot(pt - line.v0, line.v1 - line.v0)) < 1e-6f)
                                continue;

                        float w = glm::length(pt - p);

                        float at = target(uv.x * target.width, uv.y * target.height);
                        float ai = interior(uv.x * interior.width, uv.y * interior.height);
                        float delta = std::abs(ai - at);
                        
                        dp += delta * n * w * glm::sign(glm::dot(pt - p, n));
                        w_sum += w;
                }
        }

        if (w_sum < 1e-6f)
                return { glm::vec2 { 0.0f }, glm::vec2 { 0.0f } };

        dp /= w_sum;
        glm::vec2 dv0 = dp * (1.0f - t);
        glm::vec2 dv1 = dp * t;

        return { dv0, dv1 };
}

// Compositing images
void composite(Image <ColorChannel> &dst,
               const std::vector <const Image <SingleChannel> *> &srcs,
               const std::vector <glm::vec3> &colors,
               const std::vector <float> &alpha_m)
{
        assert(srcs.size() == colors.size());

        // Use src images as the alpha (and use a weighted average)
        #pragma omp parallel for
        for (int y = 0; y < dst.height; y++) {
                for (int x = 0; x < dst.width; x++) {
                        float u = x / float(dst.width);
                        float v = y / float(dst.height);

                        glm::vec3 color = dst(x, y);
                        for (int i = 0; i < srcs.size(); i++) {
                                int x_ = u * srcs[i]->width;
                                int y_ = v * srcs[i]->height;

                                float alpha = (*srcs[i])(x_, y_) * alpha_m[i];
                                if (alpha < 1e-6f)
                                        continue;

                                color = alpha * colors[i] + (1.0f - alpha) * color;
                        }

                        dst(x, y) = color;
                }
        }
}

void convert(const Image <ColorChannel> &src, uint8_t *data)
{
        #pragma omp parallel for
        for (int y = 0; y < src.height; y++) {
                for (int x = 0; x < src.width; x++) {
                        int i = (y * src.width + x) * 4;
                        int y_ = src.height - y - 1;
                        data[i + 0] = src(x, y_).r * 255.0f;
                        data[i + 1] = src(x, y_).g * 255.0f;
                        data[i + 2] = src(x, y_).b * 255.0f;
                        data[i + 3] = 255;
                }
        }
}

// Intersection time of ray and circle
float intersect(const glm::vec2 &o, const glm::vec2 &d, const glm::vec2 &c, float r)
{
        // Return -1.0f if no intersection or out of bounds (e.g. t > 1.0f)
        float a = glm::dot(d, d);
        float b = 2.0f * glm::dot(d, o - c);
        float c_ = glm::dot(o - c, o - c) - r * r;

        float delta = b * b - 4.0f * a * c_;

        if (delta < 0.0f)
                return -1.0f;

        float t0 = (-b - std::sqrt(delta)) / (2.0f * a);
        float t1 = (-b + std::sqrt(delta)) / (2.0f * a);

        float t = t0 > 0.0f ? t0 : t1;

        if (t < 0.0f || t > 1.0f)
                return -1.0f;

        return t;
}

// Intersection point of two line segments
inline float cross(const glm::vec2 &a, const glm::vec2 &b)
{
        return a.x * b.y - a.y * b.x;
}

std::optional <glm::vec2> intersection_point(const LineSegment &a, const LineSegment &b)
{
        glm::vec2 p = a.v0;
        glm::vec2 r = a.v1 - a.v0;
        glm::vec2 q = b.v0;
        glm::vec2 s = b.v1 - b.v0;

        float t = cross(q - p, s) / cross(r, s);
        float u = cross(q - p, r) / cross(r, s);

        if (t < 0.0f || t > 1.0f || u < 0.0f || u > 1.0f)
                return std::nullopt;

        return p + t * r;
}

// Gradient regularizers
void laplacian_diffuse(std::vector <glm::vec2> &grad, int power)
{
        auto copy = grad;

        for (int i = 0; i < power; i++) {
                for (int i = 0; i < grad.size(); i++) {
                        int pi = (i - 1 + grad.size()) % grad.size();
                        int ni = (i + 1) % grad.size();

                        grad[i] = (copy[pi] + 2.0f * copy[i] + copy[ni]) / 4.0f;
                }

                if (i + 1 < power)
                        std::swap(grad, copy);
        }
}

void resolve_grad_collisions(const Curve &curve, std::vector <glm::vec2> &grad)
{
        // Ensure that the gradients will not cause the curve to self-intersect
        for (int i = 0; i < curve.size(); i++) {
                int j = (i + 1) % curve.size();

                const glm::vec2 &p0 = curve[i];
                const glm::vec2 &p1 = curve[j];

                glm::vec2 g0 = grad[i];
                glm::vec2 g1 = grad[j];

                auto opt = intersection_point({ p0, p0 + g0 }, { p1, p1 + g1 });
                if (!opt)
                        continue;

                glm::vec2 p = *opt;

                // Clamp the gradients to (half) the intersection point
                // grad[i] = 0.5f * (p - p0);
                // grad[j] = 0.5f * (p - p1);
        }
}

void loop_untangle(Curve &curve, std::vector <glm::vec2> &grad)
{

}

// Optimizers
struct Optimizer {
        virtual float step(Curve &curve, const std::vector <glm::vec2> &grad) = 0;
};

struct SGD : Optimizer {
        float alpha;

        SGD(float alpha_) : alpha { alpha_ } {}

        float step(Curve &curve, const std::vector <glm::vec2> &grad) override {
                float grad_avg = 0.0f;
                for (int i = 0; i < curve.size(); i++) {
                        curve[i] += alpha * grad[i];
                        grad_avg += glm::length(grad[i]);
                }

                return grad_avg / curve.size();
        }
};

struct Momentum : Optimizer {
        std::vector <glm::vec2> v;

        float alpha;
        float eta;

        Momentum(float alpha_, float eta_ = 0.9)
                : alpha { alpha_ }, eta { eta_ } {}

        float step(Curve &curve, const std::vector <glm::vec2> &grad) override {
                if (v.size() != curve.size()) {
                        printf("Initializing momentum\n");
                        v.resize(curve.size());

                        // Clear the momentum
                        for (int i = 0; i < curve.size(); i++)
                                v[i] = glm::vec2 { 0.0f };
                }

                float grad_avg = 0.0f;
                for (int i = 0; i < curve.size(); i++) {
                        assert(!glm::any(glm::isnan(grad[i])));
                        v[i] = eta * v[i] + alpha * grad[i];
                        curve[i] += v[i];
                        grad_avg += glm::length(grad[i]);
                }

                return grad_avg / curve.size();
        }

        void upscale(const Curve &curve, const std::map <int32_t,int32_t> &new_indices) {
                printf("Upscaling momentum to %d\n", curve.size());
                std::vector <glm::vec2> new_v(curve.size());
                for (int i = 0; i < curve.size(); i++) {
                        if (new_indices.count(i) == 0) {
                                new_v[i] = v[i];
                        } else {
                                int j = new_indices.at(i);
                                int nj = (j + 1) % v.size();

                                new_v[i] = 0.5f * (v[j] + v[nj]);
                        }
                }

                v = std::move(new_v);
        }
};

struct CurveRasterizer {
        Curve curve;

        glm::vec2 min;
        glm::vec2 max;
        
        Image <SingleChannel> boundary;
        Image <SingleChannel> interior;

        CurveRasterizer(const Curve &curve_)
                        : curve { curve_ },
                        boundary { WINDOW_WIDTH, WINDOW_HEIGHT },
                        interior { WINDOW_WIDTH, WINDOW_HEIGHT } {
                std::tie(min, max) = bounds(curve);
        }

        void clear() {
                boundary.clear();
                interior.clear();
        }

        void rasterize() {
                rasterize_boundary(boundary, curve);
                rasterize_interior(interior, curve, min, max);
        }

        std::vector <glm::vec2> normal() {
                std::vector <glm::vec2> normal { curve.size(), glm::vec2 { 0.0f } };
                for (int i = 0; i < curve.size(); i++) {
                        int j = (i + 1) % curve.size();

                        glm::vec2 p0 = curve[i];
                        glm::vec2 p1 = curve[j];

                        glm::vec2 d = glm::normalize(p1 - p0);
                        glm::vec2 n = glm::normalize(glm::vec2(-d.y, d.x));

                        normal[i] = n;
                }

                return normal;
        }

        float length() {
                float length = 0.0f;
                for (int i = 0; i < curve.size(); i++) {
                        int j = (i + 1) % curve.size();

                        glm::vec2 p0 = curve[i];
                        glm::vec2 p1 = curve[j];

                        length += glm::length(p1 - p0);
                }

                return length;
        }

        std::vector <glm::vec2> grad(const Image <SingleChannel> &target, int N) {
                std::mt19937 rng { std::random_device {}() };
                std::uniform_real_distribution <float> dist { 0.0f, 1.0f };

                std::vector <glm::vec2> grad { curve.size(), glm::vec2 { 0.0f } };

                #pragma omp parallel for
                for (int i = 0; i < curve.size(); i++) {
                        int j = (i + 1) % curve.size();

                        glm::vec2 p0 = curve[i];
                        glm::vec2 p1 = curve[j];

                        glm::vec2 dv0 { 0.0f };
                        glm::vec2 dv1 { 0.0f };

                        // TODO: monte carlo estimation
                        for (int k = 0; k < N; k++) {
                                float t = dist(rng);
                                auto [sdv0, sdv1] = grad_segment(target, interior, { p0, p1 }, t);
                                
                                dv0 += sdv0;
                                dv1 += sdv1;
                        }
                        
                        dv0 /= N;
                        dv1 /= N;

                        // Account for the fact that the gradient is only one line
                        grad[i] += 0.5f * dv0;
                        grad[j] += 0.5f * dv1;
                }

                return grad;
        }

        std::map <int32_t, int32_t> adaptive_subdivide(const std::vector <glm::vec2> &grad) {
                float redist = 0.8 * length() / curve.size();

                // Subdivide based on redistributing length
                std::vector <glm::vec2> new_curve;
                std::map <int32_t, int32_t> new_indices;
                for (int i = 0; i < curve.size(); i++) {
                        int j = (i + 1) % curve.size();

                        glm::vec2 p0 = curve[i];
                        glm::vec2 p1 = curve[j];

                        float d = glm::length(p1 - p0);
                        int n = std::max(1, (int)std::round(d/redist));

                        for (int k = 0; k < n; k++) {
                                float t = (float) k / (float) n;
                                glm::vec2 p = (1.0f - t) * p0 + t * p1;

                                new_indices[new_curve.size()] = i;
                                new_curve.push_back(p);
                        }
                }

                curve = new_curve;
                update();

                printf("Subdivided to %d\n", curve.size());

                return new_indices;
        }

        void update() {
                std::tie(min, max) = bounds(curve);
        }
};

// Sliding window for average and variance
template <size_t N>
struct SlidingWindow {
        float window[N];
        int index;
        bool full;

        float mean;
        float variance;

        SlidingWindow() : index { 0 }, full { false }, mean { 0.0f }, variance { 0.0f } {
                for (int i = 0; i < N; i++)
                        window[i] = 0.0f;
        }

        void push(float value) {
                float old = window[index];
                float old_mean = mean;
                window[index] = value;

                index = (index + 1) % N;
                full = full || index == 0;
                int size = full ? N : index;

                mean += (value - old) / N;
                variance += (value - old) * (value - mean + old - old_mean) / N;
        }
};

int main(int argc, char *argv[])
{
        // TODO: option for image -> image or circle -> image
        std::string path = "../duck.jpg";
        bool invert = false;

        for (int i = 0; i < argc; i++) {
                if (std::string(argv[i]) == "--invert" || std::string(argv[i]) == "-i")
                        invert = true;
                else if (std::string(argv[i]) == "--path" || std::string(argv[i]) == "-p")
                        path = std::string(argv[i + 1]);
        }

        // Load target image
        int width, height, channels;
        stbi_set_flip_vertically_on_load(true);
        unsigned char *data = stbi_load(path.c_str(), &width, &height, &channels, 0);

        Image <SingleChannel> target_interior { width, height };
        Image <ColorChannel> target_display{ width, height };

        for (int i = 0; i < width * height; i++) {
                float r = data[channels * i + 0] / 255.0f;
                float g = data[channels * i + 1] / 255.0f;
                float b = data[channels * i + 2] / 255.0f;
                float a = channels == 4 ? data[channels * i + 3] / 255.0f : 1.0f;
                // TODO: alpha as flag (default on)

                float L = a * (0.2126f * r + 0.7152f * g + 0.0722f * b);

                int x = i % width;
                int y = i / width;

                // TODO: args for alpha smooth cutoff
                target_interior(x, y) = invert ? 1.0f - L : L;
                target_interior(x, y) = glm::smoothstep(0.0f, 0.2f, target_interior(x, y));

                // TODO: color alpha
                target_display(x, y) = { r, g, b };
        }

        // TODO: smooth the interior image
        // target_interior.smooth(3);

        if (!data) {
                fprintf(stderr, "Failed to load image: %s\n", path.c_str());
                return -1;
        }
        // TODO: sobel to get boundary

        printf("Loaded image: %s (%dx%d, %d channels)\n", path.c_str(), width, height, channels);

        // Initialize GLFW
        GLFWwindow *window = glfw_init();
        if (!window) {
                fprintf(stderr, "Failed to initialize GLFW\n");
                return -1;
        }

        // Create initial curve
        Curve curve;
        float radius = 0.5;
        for (float theta = 0.0f; theta < 2.0f * M_PI; theta += 0.01f)
                curve.push_back(glm::vec2 { radius * cos(theta), radius * sin(theta) });

        printf("Curve elements size: %lu\n", curve.size());
        CurveRasterizer source { curve };

        // Allocate images for rendering
        Image <SingleChannel> ann_grad { WINDOW_WIDTH, WINDOW_HEIGHT };
        Image <SingleChannel> ann_normal { WINDOW_WIDTH, WINDOW_HEIGHT };
        Image <ColorChannel> final { WINDOW_WIDTH, WINDOW_HEIGHT };

        // Prepare GIF writing
        GifWriter gif;
        GifBegin(&gif, "out.gif", WINDOW_WIDTH, WINDOW_HEIGHT, 2);
        uint8_t *frame = new uint8_t[WINDOW_WIDTH * WINDOW_HEIGHT * 4];

        int power = 100;
        bool show_target_alpha = false;
        bool show_normal = false;
        bool show_gradient = false;

        bool pause = false;
        bool pause_rise = false;

        std::map <int, bool> pressed;

        int iterations = 0;
        SlidingWindow <10> iou_window;
        Momentum opt = Momentum { 0.01f };

        // TODO: max number of iterations
        // TODO: ImGui
        while (!glfwWindowShouldClose(window)) {
                glfwPollEvents();
                if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
                        glfwSetWindowShouldClose(window, GL_TRUE);

                if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS && !pause_rise) {
                        pause = !pause;
                        pause_rise = true;
                } else if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_RELEASE) {
                        pause_rise = false;
                }

                if (glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS && !pressed[GLFW_KEY_T]) {
                        show_target_alpha = !show_target_alpha;
                        pressed[GLFW_KEY_T] = true;
                } else if (glfwGetKey(window, GLFW_KEY_T) == GLFW_RELEASE) {
                        pressed[GLFW_KEY_T] = false;
                }

                if (pause)
                        continue;

                // Render here
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);

                // Compute gradients and update
                final.clear(glm::vec3 {1.0f});

                source.clear();
                ann_grad.clear();
                ann_normal.clear();

                source.rasterize();
                // auto derivatives = source.grad(target.interior, 5);
                auto derivatives = source.grad(target_interior, 5);
                auto normals = source.normal();

                resolve_grad_collisions(source.curve, derivatives);
                laplacian_diffuse(derivatives, power);

                std::vector <glm::vec2> ann_points;
                std::vector <glm::vec2> ann_derivatives;
                std::vector <glm::vec2> ann_normals;

                for (int i = 0; i < source.curve.size(); i += 10) {
                        ann_points.push_back(source.curve[i]);
                        ann_derivatives.push_back(derivatives[i]);
                        ann_normals.push_back(0.02f * normals[i]);
                }

                if (show_normal)
                        quiver(ann_normal, ann_points, ann_normals);

                if (show_gradient)
                        quiver(ann_grad, ann_points, ann_derivatives);

                float grad_avg = opt.step(source.curve, derivatives);
                source.update();

                // Composite and draw
                if (!show_target_alpha)
                        final.copy(target_display);

                std::vector <const Image <SingleChannel> *> images;
                std::vector <glm::vec3> colors;
                std::vector <float> alphas;

                images.push_back(&source.boundary);
                colors.push_back({ 0.0f, 1.0f, 1.0f });
                alphas.push_back(1.0f);

                images.push_back(&source.interior);
                colors.push_back({ 0.4f, 0.5f, 0.7f });
                alphas.push_back(0.4f);

                images.push_back(&ann_grad);
                colors.push_back({ 1.0f, 0.2f, 0.2f });
                alphas.push_back(1.0f);

                images.push_back(&ann_normal);
                colors.push_back({ 0.2f, 1.0f, 0.2f });
                alphas.push_back(1.0f);

                if (show_target_alpha) {
                        images.push_back(&target_interior);
                        colors.push_back({ 0.7f, 0.8f, 0.2f });
                        alphas.push_back(0.5f);
                }

                composite(final, images, colors, alphas);
                glDrawPixels(final.width, final.height, ColorChannel::gl, GL_FLOAT, final.data);

                // Write frame to GIF
                convert(final, frame);
                GifWriteFrame(&gif, frame, WINDOW_WIDTH, WINDOW_HEIGHT, 2);

                // Print info
                float metric = iou(target_interior, source.interior);
                // printf("\033[2J\033[1;1H");
                // printf("IOU: %f\n", metric);

                if (metric > 0.85)
                        power = 20; // TODO: also depends on the curve size (and
                // variance of grdients)
                if (metric > 0.95)
                        power = 1;

                // Subdivide the curve if needed
                iou_window.push(100.0f * metric);

                // printf("IOU: %f, mean: %f, variance: %f\n", metric, iou_window.mean, iou_window.variance);

                if ((++iterations) % 50 == 0) {
                        printf("Subdividing, gradient average: %f\n", grad_avg);
                        auto new_indices = source.adaptive_subdivide(derivatives);
                        opt.upscale(source.curve, new_indices);
                }

                // Swap front and back buffers
                glfwSwapBuffers(window);
        }

        // Finish GIF writing
        GifEnd(&gif);
}

GLFWwindow *glfw_init()
{
	// Basic window
	GLFWwindow *window = nullptr;
	if (!glfwInit())
		return nullptr;

	window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Murphy", NULL, NULL);

	// Check if window was created
	if (!window) {
		glfwTerminate();
		return nullptr;
	}

	// Make the window's context current
	glfwMakeContextCurrent(window);

	// Load OpenGL functions using GLAD
	if (!gladLoadGLLoader((GLADloadproc) glfwGetProcAddress)) {
		fprintf(stderr, "Failed to initialize OpenGL context\n");
		return nullptr;
	}

	// Set up callbacks
	// glfwSetCursorPosCallback(window, mouse_callback);
	// glfwSetMouseButtonCallback(window, mouse_button_callback);
	// glfwSetKeyCallback(window, keyboard_callback);

	const GLubyte* renderer = glGetString(GL_RENDERER);
	printf("Renderer: %s\n", renderer);

	return window;
}
