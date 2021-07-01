#include "lnf/box.h"
#include "lnf/bvh.h"
#include "lnf/color.h"
#include "lnf/constants.h"
#include "lnf/default_materials.h"
#include "lnf/marched_materials.h"
#include "lnf/frame.h"
#include "lnf/jobs.h"
#include "lnf/jpeg.h"
#include "lnf/mandlebrot.h"
#include "lnf/marched_bubbles.h"
#include "lnf/marched_mandle.h"
#include "lnf/marched_sphere.h"
#include "lnf/mesh.h"
#include "lnf/outputimage.h"
#include "lnf/plane.h"
#include "lnf/profile.h"
#include "lnf/ray.h"
#include "lnf/scene.h"
#include "lnf/smoke_box.h"
#include "lnf/sphere.h"
#include "lnf/trace.h"
#include "lnf/uv.h"
#include "lnf/vec3.h"
#include "lnf/viewport.h"

#include <vector>
#include <memory>
#include <random>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <iostream>
#include <string>

#include <QtWidgets>



using namespace LNF;


// diffuse material
class DiffuseMandlebrot : public Diffuse
{
 public:
    DiffuseMandlebrot()
        :Diffuse(Color()),
         m_mandlebrot(1, 1),
         m_baseColor(0.4f, 0.2f, 0.1f)
    {}
    
    /* Returns the diffuse color at the given surface position */
    virtual Color color(const Intersect &_hit) const override {
        return m_baseColor * (m_mandlebrot.value(_hit.m_uv.u(), _hit.m_uv.v()) * 0.1f + 0.1f);
    }
    
 private:
    MandleBrot      m_mandlebrot;
    Color           m_baseColor;
};


// simple scene with a linear search for object hits
class SimpleScene   : public Scene
{
 public:
    SimpleScene()
    {}
        
    /*
       Checks for an intersect with a scene object.
       Could be accessed by multiple worker threads concurrently.
     */
    virtual bool hit(Intersect &_hit) const override {
        for (const auto &pObj : m_objects) {
            Intersect nh(_hit);
            if ( (pObj->hit(nh) == true) &&
                 ( (_hit == false) || (nh.m_fPositionOnRay < _hit.m_fPositionOnRay)) )
            {
                _hit = nh;
            }
        }
        
        return _hit;
    }
    
    /*
     Checks for the background color (miss handler).
     Could be accessed by multiple worker threads concurrently.
     */
    virtual Color backgroundColor() const override {
        return Color(0.2f, 0.2f, 0.2f);
    }

    /*
     Add a new resource (material, primitive, instance) to the scene.
     May not be safe to call while worker threads are calling 'hit'/
    */
    virtual Resource *addResource(std::unique_ptr<Resource> &&_pResource) override {
        m_resources.push_back(std::move(_pResource));
        return m_resources.back().get();
    }

    /*
     Add a new primitive instance to the scene.
     May not be safe to call while worker threads are calling 'hit'/
     */
    virtual PrimitiveInstance *addPrimitiveInstance(std::unique_ptr<PrimitiveInstance> &&_pInstance) override {
        m_objects.push_back(std::move(_pInstance));
        return m_objects.back().get();
    }
    
 protected:
    std::vector<std::unique_ptr<Resource>>           m_resources;
    std::vector<std::unique_ptr<PrimitiveInstance>>  m_objects;
};


// simple scene using a BVH for optimising hits
class SimpleSceneBvh   : public SimpleScene
{
 public:
    SimpleSceneBvh()
    {}
        
    // Checks for an intersect with a scene object (could be accessed by multiple worker threads concurrently).
    virtual bool hit(Intersect &_hit) const override {
        return checkBvhHit(_hit, m_root);
    }

    // Build acceleration structures
    void build() {
        std::vector<const PrimitiveInstance*> rawObjects(m_objects.size(), nullptr);
        for (size_t i = 0; i < m_objects.size(); i++) {
            rawObjects[i] = m_objects[i].get();
        }

        m_root = buildBvhRoot<2>(rawObjects, 16);
    }

 private:
    // Search for best hit through BVHs
    bool checkBvhHit(Intersect &_hit, const std::unique_ptr<BvhNode<PrimitiveInstance>> &_pNode) const
    {
        if (_pNode->empty() == false) {
            for (const auto &pObj : _pNode->m_primitives) {
                Intersect nh(_hit);
                if ( (pObj->hit(nh) == true) &&
                     ( (_hit == false) || (nh.m_fPositionOnRay < _hit.m_fPositionOnRay)) )
                {
                    _hit = nh;
                }
            }
        }

        if ( (_pNode->m_left != nullptr) &&
             (_pNode->m_left->intersect(_hit.m_viewRay) == true) ) {
            checkBvhHit(_hit, _pNode->m_left);
        }
        
        if ( (_pNode->m_right != nullptr) &&
             (_pNode->m_right->intersect(_hit.m_viewRay) == true) ) {
            checkBvhHit(_hit, _pNode->m_right);
        }
        
        return _hit;
    }
    
 protected:
    std::unique_ptr<BvhNode<PrimitiveInstance>>      m_root;
};


class MainWindow : public QMainWindow
{
 protected:
    using clock_type = std::chrono::high_resolution_clock;
    
 public:
    MainWindow(const SimpleScene *_pScene)
        :QMainWindow(),
         m_pScene(_pScene),
         m_iFrameCount(0),
         m_bFrameDone(false),
         m_iWidth(1024),
         m_iHeight(768),
         m_fFov(60),
         m_iNumWorkers(std::max(std::thread::hardware_concurrency() * 2, 2u)),
         m_iMaxSamplesPerPixel(64),
         m_iMaxTraceDepth(16),
         m_fColorTollerance(0.000000001f)
    {
        resize(m_iWidth, m_iHeight);
        setWindowTitle(QApplication::translate("windowlayout", "Raytracer"));
        startTimer(200, Qt::PreciseTimer);
        
        m_pView = std::make_unique<Viewport>(m_iWidth, m_iHeight);
        m_pCamera = std::make_unique<SimpleCamera>(Vec(0, 60, 200), Vec(0, 1, 0), Vec(0, 5, 0), deg2rad(m_fFov), 1.5, 120);
        m_pView->setCamera(m_pCamera.get());
    }
    
 protected:
    virtual void paintEvent(QPaintEvent *_event) {
        QPainter painter(this);
        QImage image(m_iWidth, m_iHeight, QImage::Format_RGB888);

        if (m_pSource != nullptr) {
            std::memcpy(image.bits(),
                        m_pSource->image().data(),
                        m_pSource->image().size());
        }
        else {
            image.fill(Qt::black);
        }
        
        painter.drawImage(0, 0, image);
        m_iFrameCount++;
    }
    
    virtual void timerEvent(QTimerEvent *_event) {
        if (m_pSource == nullptr)
        {
            m_tpInit = clock_type::now();
            m_pSource = std::make_unique<Frame>(m_pView.get(),
                                                m_pScene,
                                                m_iNumWorkers,
                                                m_iMaxSamplesPerPixel,
                                                m_iMaxTraceDepth,
                                                m_fColorTollerance);
        }
        else {
            m_pSource->updateFrameProgress();
            printf("active jobs=%d, progress=%.2f, time_to_finish=%.2fs, total_time=%.2fs, rays_ps=%.2f\n",
                    (int)m_pSource->activeJobs(), m_pSource->progress(), m_pSource->timeToFinish(), m_pSource->timeTotal(), m_pSource->raysPerSecond());
            
            if (m_pSource->isFinished() == true) {
                if (m_bFrameDone == false) {
                    m_pSource->writeJpegFile("raytraced.jpeg", 100);
                    m_bFrameDone = true;
                    
                    auto td = clock_type::now() - m_tpInit;
                    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(td).count();
                    
                    std::string title = std::string("Done ") + std::to_string((float)ns/1e09) + "s";
                    setWindowTitle(QString::fromStdString(title));


                    /*
                    printf("time on bvh = %.2f, time on hits = %.2f\n",
                            (float)m_pScene->getTimeOnBvhS()/m_iNumWorkers,
                            (float)m_pScene->getTimeOnHitsS()/m_iNumWorkers);
                    */



                }
            }
        }
        
        this->update(this->rect());
    }
    
 private:
    const SimpleScene                   *m_pScene;
    std::unique_ptr<Viewport>           m_pView;
    std::unique_ptr<Camera>             m_pCamera;
    std::unique_ptr<LNF::Frame>         m_pSource;
    int                                 m_iFrameCount;
    bool                                m_bFrameDone;
    clock_type::time_point              m_tpInit;
    int                                 m_iWidth;
    int                                 m_iHeight;
    float                               m_fFov;
    int                                 m_iNumWorkers;
    int                                 m_iMaxSamplesPerPixel;
    int                                 m_iMaxTraceDepth;
    float                               m_fColorTollerance;
};



int main(int argc, char *argv[])
{
    // init
    auto pScene = std::make_unique<SimpleSceneBvh>();
    
    // create scene
    
    /*
    auto pDiffuseFloor = createMaterial<DiffuseCheckered>(pScene.get(), Color(1.0, 1.0, 1.0), Color(1.0, 0.4, 0.2), 2);
    //auto pDiffuseFog = createMaterial<Diffuse>(pScene.get(), Color(0.9, 0.9, 0.9));
    auto pGlass = createMaterial<Glass>(pScene.get(), Color(0.95, 0.95, 0.95), 0.01, 1.8);
    auto pMirror = createMaterial<Metal>(pScene.get(), Color(0.95, 0.95, 0.95), 0.02);
    auto pAO = createMaterial<FakeAmbientOcclusion>(pScene.get());
    auto pMetalIt = createMaterial<MetalIterations>(pScene.get());
    auto pGlow = createMaterial<Glow>(pScene.get());
    auto pLightWhite = createMaterial<Light>(pScene.get(), Color(30.0, 30.0, 30.0));

    createPrimitiveInstance<Disc>(pScene.get(), axisIdentity(), 500, pDiffuseFloor);
    createPrimitiveInstance<Rectangle>(pScene.get(), axisTranslation(Vec(0, 1, 0)), 200, 200, pMirror);
    //createPrimitiveInstance<SmokeBox>(pScene.get(), axisIdentity(), 400, pDiffuseFog, 400);
    createPrimitiveInstance<Sphere>(pScene.get(), axisTranslation(Vec(0, 200, 100)), 30, pLightWhite);
    createPrimitiveInstance<MarchedMandle>(pScene.get(), axisEulerZYX(0, 1, 0, Vec(-50, 45, 50), 40.0), pGlow);
    createPrimitiveInstance<MarchedSphere>(pScene.get(), axisEulerZYX(0, 1, 0, Vec(50, 45, 50), 40.0), 2.0f, pGlass, 0.04f);
    createPrimitiveInstance<MarchedBubbles>(pScene.get(), axisEulerZYX(0, 1, 0, Vec(0, 45, -50), 40.0), 2.0f, pGlass);
    */
    

    auto pDiffuseRed = createMaterial<Diffuse>(pScene.get(), Color(0.9f, 0.1f, 0.1f));
    auto pDiffuseGreen = createMaterial<Diffuse>(pScene.get(), Color(0.1f, 0.9f, 0.1f));
    auto pDiffuseBlue = createMaterial<Diffuse>(pScene.get(), Color(0.1f, 0.1f, 0.9f));
    
    auto pMesh1 = createPrimitive<SphereMesh>(pScene.get(), 16, 16, 4, pDiffuseRed);
    auto pMesh2 = createPrimitive<SphereMesh>(pScene.get(), 16, 16, 4, pDiffuseGreen);
    auto pMesh3 = createPrimitive<SphereMesh>(pScene.get(), 16, 16, 4, pDiffuseBlue);

    auto pSphere1 = createPrimitive<Sphere>(pScene.get(), 4, pDiffuseRed);
    auto pSphere2 = createPrimitive<Sphere>(pScene.get(), 4, pDiffuseGreen);
    auto pSphere3 = createPrimitive<Sphere>(pScene.get(), 4, pDiffuseBlue);
    
    auto pLightWhite = createMaterial<Light>(pScene.get(), Color(10.0f, 10.0f, 10.0f));
    createPrimitiveInstance<Sphere>(pScene.get(), axisTranslation(Vec(0, 200, 100)), 30, pLightWhite);
    
    auto shapes = std::vector{pSphere1, pSphere2, pSphere3};
    //auto shapes = std::vector{pMesh1, pMesh2, pMesh3};
    
    int n = 200;
    for (int i = 0; i < n; i++) {
        
        float x = 100 * sin((float)i / n * LNF::pi * 2);
        float y = 20 * (cos((float)i / n * LNF::pi * 16) + 1);
        float z = 100 * cos((float)i / n * LNF::pi * 2);

        //createPrimitiveInstance<Sphere>(pScene.get(), axisEulerZYX(0, 0, 0, Vec(x, y, z)), 4, pDiffuseRed);
        //createPrimitiveInstance<SphereMesh>(pScene.get(), axisEulerZYX(0, 0, 0, Vec(x, y, z)), 32, 16, 4, pDiffuseGreen);
        createPrimitiveInstance(pScene.get(), axisEulerZYX(0, 0, 0, Vec(x, y, z)), shapes[i % shapes.size()]);
    }

    
    pScene->build();

    // start app
    QApplication app(argc, argv);
    MainWindow window(pScene.get());
    window.show();

    return app.exec();
}
