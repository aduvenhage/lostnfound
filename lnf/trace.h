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
    /* Trace ray (recursively) through scene */
    Color trace(const Ray &_ray,
                const Scene *_pScene,
                RandomGen &_randomGen,
                int _iMaxTraceDepth)
    {
        Intersect hit;
        if (_pScene->hit(hit, _ray) == true) {
            if (hit.m_pNode != nullptr) {
            
                // complete intercept (
                auto pHitNode = hit.m_pNode;
                pHitNode->intersect(hit);
            
                // create scattered, reflected, reftracted, etc. color
                auto pMaterial = pHitNode->material();
                auto scatteredRay = pMaterial->scatter(hit, _randomGen);
                scatteredRay.m_ray.m_origin = scatteredRay.m_ray.position(1e-4);
                
                // transform ray back to world space
                scatteredRay.m_ray.m_direction = hit.m_axis.rotateFrom(scatteredRay.m_ray.m_direction);
                scatteredRay.m_ray.m_origin = hit.m_axis.transformFrom(scatteredRay.m_ray.m_origin);

                // trace recursively and blend colors
                if ( (_iMaxTraceDepth > 0) && (scatteredRay.m_color.isBlack() == false) ) {
                    auto tracedColor = trace(scatteredRay.m_ray, _pScene, _randomGen, _iMaxTraceDepth - 1);
                    return scatteredRay.m_emitted + scatteredRay.m_color * tracedColor;
                }
                else {
                    return scatteredRay.m_emitted;
                }
            }
        }
        
        return _pScene->missColor(_ray);  // background color
    }


    /* raytrace for a specific view to a specific output block */
    void renderImage(OutputImage *_pOutput,
                     const Viewport *_pView,
                     const Scene *_scene,
                     RandomGen &_generator,
                     int _iRaysPerPixel, int _iMaxDepth)
    {
        // create rays and trace them for all pixels in block
        for (auto j = 0; j < _pOutput->height(); j++)
        {
            unsigned char *pPixel = _pOutput->row(j);
            for (auto i = 0; i < _pOutput->width(); i++)
            {
                auto color = Color();
                for (int k = 0; k < _iRaysPerPixel; k++)
                {
                    auto ray = _pView->getRay(i, j, _generator);
                    color += trace(ray, _scene, _generator, _iMaxDepth - 1);
                }
                                
                // write averaged color to output image
                color = (color / _iRaysPerPixel).clamp();
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
    bool marchedTrace(Vec &_intersect,
                      Vec &_normal,
                      const Ray &_ray,
                      const sdf_func &_sdf,
                      float _fStepScale,
                      float _fMaxDist)
    {
        const float e = 0.00001;
        const int n = 1000;
        _intersect = _ray.m_origin;
        
        for (int i = 0; i < n; i++) {
            float distance = _sdf(_intersect);
            if (distance > _fMaxDist) {
                return false;   // missed
            }
            else if (fabs(distance) <= e) {
                _normal = marchedNormal(_intersect, _sdf);
                return true;    // hit
            }
            else if (distance < 0) {
                _intersect = _intersect + _ray.m_direction * distance * _fStepScale * 0.2;
            }
            else {
                _intersect = _intersect + _ray.m_direction * distance * _fStepScale;
            }
        }
        
        return false;
    }
    

};  // namespace LNF

#endif  // #ifndef LIBS_HEADER_TRACE_H

