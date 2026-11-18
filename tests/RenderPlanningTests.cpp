#include "RenderPlanning.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
void expect(bool b) { if (!b) throw std::runtime_error("Render planning regression."); }
int main() {
    try {
        RenderSettings s; s.frustumPreview = true;
        Cluster inside{0, 3, {-.1f, -.1f, .2f}, {.1f, .1f, .3f}};
        expect(clusterVisible(inside, 0, glm::mat4(1), {0, 0, 2}, s));
        for (const auto offset : {glm::vec3(2,0,0), glm::vec3(-2,0,0), glm::vec3(0,2,0),
                                 glm::vec3(0,-2,0), glm::vec3(0,0,2), glm::vec3(0,0,-2)}) {
            auto c = inside; c.boundsMin += offset; c.boundsMax += offset;
            expect(!clusterVisible(c, 0, glm::mat4(1), {0,0,2}, s));
            expect(clusterVisible(c, 3, glm::mat4(1), {0,0,2}, s));
        }
        s.frustumPreview = false; s.maxDistance = .01f;
        expect(!clusterVisible(inside, 0, glm::mat4(1), {0,0,2}, s));
        expect(clusterVisible(inside, 3, glm::mat4(1), {0,0,2}, s));
        for (auto direction : {glm::vec3(0,1,0), glm::vec3(0,0,1), glm::vec3(.6f,.8f,1)}) {
            const auto light = shadowViewProjection({inside}, .6f, direction);
            const auto half = shadowViewProjection({inside}, .6f, direction, .5f);
            for (int i=0; i<8; ++i) {
                auto lo=inside.boundsMin-glm::vec3(.6f), hi=inside.boundsMax+glm::vec3(.6f);
                const auto corner = glm::vec4((i&1)?hi.x:lo.x,(i&2)?hi.y:lo.y,(i&4)?hi.z:lo.z,1);
                auto p=light*corner;
                p/=p.w; expect(std::abs(p.x)<=1 && std::abs(p.y)<=1 && p.z>=0 && p.z<=1);
                const auto cropped = half * corner;
                expect(glm::length(glm::vec2(cropped) - 2.f * glm::vec2(p)) < 1e-5f && cropped.z == p.z);
            }
        }
        bool rejected=false; s.lightDirection={0,0,0};
        try { validateRenderSettings(s); } catch(const std::exception&) { rejected=true; }
        expect(rejected);
        std::cout << "Render planning: clip planes, expanded bounds and light fit passed.\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
