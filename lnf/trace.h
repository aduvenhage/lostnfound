#ifndef LIBS_HEADER_TRACE_H
#define LIBS_HEADER_TRACE_H


#include "color.h"
#include "constants.h"
#include "intersect.h"
#include "material.h"
#include "outputimage.h"
#include "ray.h"
#include "scene.h"
#include "uv.h"
#include "vec3.h"
#include "viewport.h"

#include <vector>
#include <random>
#include <atomic>
#include <functional>


namespace LNF
{
    class Tracer
    {
     public:
        Tracer(const Scene *_pScene, RandomGen &_randomGen, int _iMaxTraceDepth)
            :m_pScene(_pScene),
             m_randomGen(_randomGen),
             m_iTraceLimit(_iMaxTraceDepth),
             m_iTraceDepth(0),
             m_iTraceDepthMax(0)
        {}
        
        Color trace(const Ray &_ray) {
            m_iTraceDepth = 0;
            return traceRay(_ray);
        }
        
        int traceDepth() const {return m_iTraceDepth;}
        int traceDepthMax() const {return m_iTraceDepthMax;}

     protected:
        /* Trace ray (recursively) through scene */
        Color traceRay(const Ray &_ray) {
            m_iTraceDepth++;
            m_iTraceDepthMax = std::max(m_iTraceDepth, m_iTraceDepthMax);
            
            Intersect hit;
            if (m_pScene->hit(hit, _ray) == true) {
                if (hit.m_pNode != nullptr) {
                
                    // complete intercept (
                    auto pHitNode = hit.m_pNode;
                    pHitNode->intersect(hit);
                
                    // create scattered, reflected, refracted, etc. ray and color
                    auto scatteredRay = pHitNode->material()->scatter(hit, m_randomGen);
                    
                    // trace recursively and blend colors
                    if ( (m_iTraceDepth < m_iTraceLimit) && (scatteredRay.m_color.isBlack() == false) ) {
                        // move slightly to avoid self intersection
                        scatteredRay.m_ray.m_origin = scatteredRay.m_ray.position(1e-4);
                        
                        // transform ray back to world space
                        scatteredRay.m_ray = Ray(hit.m_axis.transformFrom(scatteredRay.m_ray.m_origin),
                                                 hit.m_axis.rotateFrom(scatteredRay.m_ray.m_direction));

                        // trace again
                        auto tracedColor = traceRay(scatteredRay.m_ray);
                        return scatteredRay.m_emitted + scatteredRay.m_color * tracedColor;
                    }
                    else {
                        // max trace depth reached
                        return scatteredRay.m_emitted;
                    }
                }
            }
            
            return m_pScene->missColor(_ray);  // background color
        }
        
     private:
        const Scene     *m_pScene;
        RandomGen       &m_randomGen;
        int             m_iTraceLimit;
        int             m_iTraceDepth;
        int             m_iTraceDepthMax;
    };


    /* raytrace for a specific view to a specific output block */
    void renderImage(OutputImage *_pOutput,
                     const Viewport *_pView,
                     const Scene *_scene,
                     RandomGen &_generator,
                     int _iRaysPerPixel, int _iMaxDepth)
    {
        Tracer tracer(_scene, _generator, _iMaxDepth);

        // create rays and trace them for all pixels in block
        for (auto j = 0; j < _pOutput->height(); j++)
        {
            unsigned char *pPixel = _pOutput->row(j);
            for (auto i = 0; i < _pOutput->width(); i++)
            {
                auto stats = ColorStat();
                
                for (int k = 0; k < _iRaysPerPixel; k++)
                {
                    auto ray = _pView->getRay(i, j, _generator);
                    auto color = tracer.trace(ray);
                    stats.push(color);
                    
                    if ( (k > 4) && (k > tracer.traceDepthMax()) ) {
                        if (stats.maturity() < 0.000001)
                        {
                            break;
                        }
                    }
                }
                                
                // write averaged color to output image
                auto color = stats.mean().clamp();
                *(pPixel++) = (int)(255 * color.red() + 0.5);
                *(pPixel++) = (int)(255 * color.green() + 0.5);
                *(pPixel++) = (int)(255 * color.blue() + 0.5);
            }
        }
    }
    
    
    // get marched normal from surface function
    template <typename sdf_func>
    Vec marchedNormal(const Vec &_intersect, const sdf_func &_sdf) {
        const float e = 0.0001;
        return Vec(_sdf(_intersect + Vec{e, 0, 0}) - _sdf(_intersect - Vec{e, 0, 0}),
                   _sdf(_intersect + Vec{0, e, 0}) - _sdf(_intersect - Vec{0, e, 0}),
                   _sdf(_intersect + Vec{0, 0, e}) - _sdf(_intersect - Vec{0, 0, e})).normalized();
    }


    // ray marching on provided signed distance function
    template <typename sdf_func>
    bool marchedTrace(Vec &_intersect,      // point on surface
                      Vec &_normal,         // surface normal
                      bool &_bInside,       // false if ray is entering surface; true if ray is exiting
                      const Ray &_ray,
                      const sdf_func &_sdf)
    {
        const float MAX_DIST = 1000;
        const float e = 0.000001;
        const int n = 1000;
        float stepScale = 1.0;
        
        _bInside = false;
        _intersect = _ray.m_origin;
        float distance = _sdf(_intersect);
        if (distance < 0) {
            _bInside = true;
            stepScale *= -1;
        }
        
        for (int i = 0; i < n; i++) {
            // check hit or miss
            float absd = fabs(distance);
            if (absd > MAX_DIST) {
                _bInside = distance < 0;
                return false;   // missed
            }
            else if (absd <= e) {
                _normal = marchedNormal(_intersect, _sdf);
                return true;    // hit
            }
            
            // move forward
            _intersect = _intersect + _ray.m_direction * distance * stepScale;
            
            // check for surface crossing
            float newDistance = _sdf(_intersect);
            if (distance * newDistance < -e) {
                stepScale *= 0.5;
            }
            
            distance = newDistance;
        }
        
        return false;
    }
    

};  // namespace LNF

#endif  // #ifndef LIBS_HEADER_TRACE_H

