// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include "flutter/shell/common/rasterizer.h"

#include <memory>

#include "flutter/flow/frame_timings.h"
#include "flutter/fml/synchronization/count_down_latch.h"
#include "flutter/fml/time/time_point.h"
#include "flutter/shell/common/thread_host.h"
#include "flutter/testing/testing.h"

#include "gmock/gmock.h"

using testing::_;
using testing::ByMove;
using testing::Return;
using testing::ReturnRef;

namespace flutter {
namespace {
class MockDelegate : public Rasterizer::Delegate {
 public:
  MOCK_METHOD1(OnFrameRasterized, void(const FrameTiming& frame_timing));
  MOCK_METHOD0(GetFrameBudget, fml::Milliseconds());
  MOCK_CONST_METHOD0(GetLatestFrameTargetTime, fml::TimePoint());
  MOCK_CONST_METHOD0(GetTaskRunners, const TaskRunners&());
  MOCK_CONST_METHOD0(GetParentRasterThreadMerger,
                     const fml::RefPtr<fml::RasterThreadMerger>());
  MOCK_CONST_METHOD0(GetIsGpuDisabledSyncSwitch,
                     std::shared_ptr<const fml::SyncSwitch>());
  MOCK_METHOD0(CreateSnapshotSurface, std::unique_ptr<Surface>());
};

class MockSurface : public Surface {
 public:
  MOCK_METHOD0(IsValid, bool());
  MOCK_METHOD1(AcquireFrame,
               std::unique_ptr<SurfaceFrame>(const SkISize& size));
  MOCK_CONST_METHOD0(GetRootTransformation, SkMatrix());
  MOCK_METHOD0(GetContext, GrDirectContext*());
  MOCK_METHOD0(GetExternalViewEmbedder, ExternalViewEmbedder*());
  MOCK_METHOD0(MakeRenderContextCurrent, std::unique_ptr<GLContextResult>());
  MOCK_METHOD0(ClearRenderContext, bool());
  MOCK_CONST_METHOD0(AllowsDrawingWhenGpuDisabled, bool());
};

class MockExternalViewEmbedder : public ExternalViewEmbedder {
 public:
  MOCK_METHOD0(GetRootCanvas, SkCanvas*());
  MOCK_METHOD0(CancelFrame, void());
  MOCK_METHOD4(BeginFrame,
               void(SkISize frame_size,
                    GrDirectContext* context,
                    double device_pixel_ratio,
                    fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger));
  MOCK_METHOD2(PrerollCompositeEmbeddedView,
               void(int view_id, std::unique_ptr<EmbeddedViewParams> params));
  MOCK_METHOD1(PostPrerollAction,
               PostPrerollResult(
                   fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger));
  MOCK_METHOD0(GetCurrentCanvases, std::vector<SkCanvas*>());
  MOCK_METHOD1(CompositeEmbeddedView, SkCanvas*(int view_id));
  MOCK_METHOD2(SubmitFrame,
               void(GrDirectContext* context,
                    std::unique_ptr<SurfaceFrame> frame));
  MOCK_METHOD2(EndFrame,
               void(bool should_resubmit_frame,
                    fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger));
  MOCK_METHOD0(SupportsDynamicThreadMerging, bool());
};
}  // namespace

TEST(RasterizerTest, create) {
  MockDelegate delegate;
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  EXPECT_TRUE(rasterizer != nullptr);
}

static std::unique_ptr<FrameTimingsRecorder> CreateFinishedBuildRecorder(
    fml::TimePoint timestamp) {
  std::unique_ptr<FrameTimingsRecorder> recorder =
      std::make_unique<FrameTimingsRecorder>();
  recorder->RecordVsync(timestamp, timestamp);
  recorder->RecordBuildStart(timestamp);
  recorder->RecordBuildEnd(timestamp);
  return recorder;
}

static std::unique_ptr<FrameTimingsRecorder> CreateFinishedBuildRecorder() {
  return CreateFinishedBuildRecorder(fml::TimePoint::Now());
}

TEST(RasterizerTest, drawEmptyPipeline) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  MockDelegate delegate;
  ON_CALL(delegate, GetTaskRunners()).WillByDefault(ReturnRef(task_runners));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<MockSurface>();
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));
  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<LayerTreePipeline>(/*depth=*/10);
    rasterizer->Draw(pipeline, nullptr);
    latch.Signal();
  });
  latch.Wait();
}

TEST(RasterizerTest,
     drawWithExternalViewEmbedderExternalViewEmbedderSubmitFrameCalled) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  MockDelegate delegate;
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<MockSurface>();

  std::shared_ptr<MockExternalViewEmbedder> external_view_embedder =
      std::make_shared<MockExternalViewEmbedder>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;

  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/nullptr, framebuffer_info,
      /*submit_callback=*/[](const SurfaceFrame&, SkCanvas*) { return true; });
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(true));
  EXPECT_CALL(*surface, AcquireFrame(SkISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  EXPECT_CALL(*external_view_embedder,
              BeginFrame(/*frame_size=*/SkISize(), /*context=*/nullptr,
                         /*device_pixel_ratio=*/2.0,
                         /*raster_thread_merger=*/
                         fml::RefPtr<fml::RasterThreadMerger>(nullptr)))
      .Times(1);
  EXPECT_CALL(*external_view_embedder, SubmitFrame).Times(1);
  EXPECT_CALL(
      *external_view_embedder,
      EndFrame(/*should_resubmit_frame=*/false,
               /*raster_thread_merger=*/fml::RefPtr<fml::RasterThreadMerger>(
                   nullptr)))
      .Times(1);

  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<LayerTreePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(/*frame_size=*/SkISize(),
                                                  /*device_pixel_ratio=*/2.0f);
    auto layer_tree_item = std::make_unique<LayerTreeItem>(
        std::move(layer_tree), CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    auto no_discard = [](LayerTree&) { return false; };
    rasterizer->Draw(pipeline, no_discard);
    latch.Signal();
  });
  latch.Wait();
}

TEST(
    RasterizerTest,
    drawWithExternalViewEmbedderAndThreadMergerNotMergedExternalViewEmbedderSubmitFrameNotCalled) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  MockDelegate delegate;
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<MockSurface>();
  std::shared_ptr<MockExternalViewEmbedder> external_view_embedder =
      std::make_shared<MockExternalViewEmbedder>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);
  EXPECT_CALL(*external_view_embedder, SupportsDynamicThreadMerging)
      .WillRepeatedly(Return(true));
  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;
  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/nullptr, framebuffer_info,
      /*submit_callback=*/[](const SurfaceFrame&, SkCanvas*) { return true; });
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(true));
  EXPECT_CALL(*surface, AcquireFrame(SkISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  EXPECT_CALL(*external_view_embedder,
              BeginFrame(/*frame_size=*/SkISize(), /*context=*/nullptr,
                         /*device_pixel_ratio=*/2.0,
                         /*raster_thread_merger=*/_))
      .Times(1);
  EXPECT_CALL(*external_view_embedder, SubmitFrame).Times(0);
  EXPECT_CALL(*external_view_embedder, EndFrame(/*should_resubmit_frame=*/false,
                                                /*raster_thread_merger=*/_))
      .Times(1);

  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<LayerTreePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(/*frame_size=*/SkISize(),
                                                  /*device_pixel_ratio=*/2.0f);
    auto layer_tree_item = std::make_unique<LayerTreeItem>(
        std::move(layer_tree), CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    auto no_discard = [](LayerTree&) { return false; };
    rasterizer->Draw(pipeline, no_discard);
    latch.Signal();
  });
  latch.Wait();
}

TEST(
    RasterizerTest,
    drawWithExternalViewEmbedderAndThreadsMergedExternalViewEmbedderSubmitFrameCalled) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  fml::MessageLoop::EnsureInitializedForCurrentThread();
  TaskRunners task_runners("test",
                           fml::MessageLoop::GetCurrent().GetTaskRunner(),
                           fml::MessageLoop::GetCurrent().GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());

  MockDelegate delegate;
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));

  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<MockSurface>();

  std::shared_ptr<MockExternalViewEmbedder> external_view_embedder =
      std::make_shared<MockExternalViewEmbedder>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;

  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/nullptr, framebuffer_info,
      /*submit_callback=*/[](const SurfaceFrame&, SkCanvas*) { return true; });
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(true));
  EXPECT_CALL(*surface, AcquireFrame(SkISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));
  EXPECT_CALL(*external_view_embedder, SupportsDynamicThreadMerging)
      .WillRepeatedly(Return(true));

  EXPECT_CALL(*external_view_embedder,
              BeginFrame(/*frame_size=*/SkISize(), /*context=*/nullptr,
                         /*device_pixel_ratio=*/2.0,
                         /*raster_thread_merger=*/_))
      .Times(1);
  EXPECT_CALL(*external_view_embedder, SubmitFrame).Times(1);
  EXPECT_CALL(*external_view_embedder, EndFrame(/*should_resubmit_frame=*/false,
                                                /*raster_thread_merger=*/_))
      .Times(1);

  rasterizer->Setup(std::move(surface));

  auto pipeline = std::make_shared<LayerTreePipeline>(/*depth=*/10);
  auto layer_tree = std::make_unique<LayerTree>(/*frame_size=*/SkISize(),
                                                /*device_pixel_ratio=*/2.0f);
  auto layer_tree_item = std::make_unique<LayerTreeItem>(
      std::move(layer_tree), CreateFinishedBuildRecorder());
  PipelineProduceResult result =
      pipeline->Produce().Complete(std::move(layer_tree_item));
  EXPECT_TRUE(result.success);
  auto no_discard = [](LayerTree&) { return false; };
  rasterizer->Draw(pipeline, no_discard);
}

TEST(RasterizerTest,
     drawLastLayerTreeWithThreadsMergedExternalViewEmbedderAndEndFrameCalled) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  fml::MessageLoop::EnsureInitializedForCurrentThread();
  TaskRunners task_runners("test",
                           fml::MessageLoop::GetCurrent().GetTaskRunner(),
                           fml::MessageLoop::GetCurrent().GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());

  MockDelegate delegate;
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));

  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<MockSurface>();

  std::shared_ptr<MockExternalViewEmbedder> external_view_embedder =
      std::make_shared<MockExternalViewEmbedder>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;

  auto surface_frame1 = std::make_unique<SurfaceFrame>(
      /*surface=*/nullptr, framebuffer_info,
      /*submit_callback=*/[](const SurfaceFrame&, SkCanvas*) { return true; });
  auto surface_frame2 = std::make_unique<SurfaceFrame>(
      /*surface=*/nullptr, framebuffer_info,
      /*submit_callback=*/[](const SurfaceFrame&, SkCanvas*) { return true; });
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled())
      .WillRepeatedly(Return(true));
  // Prepare two frames for Draw() and DrawLastLayerTree().
  EXPECT_CALL(*surface, AcquireFrame(SkISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame1))))
      .WillOnce(Return(ByMove(std::move(surface_frame2))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));
  EXPECT_CALL(*external_view_embedder, SupportsDynamicThreadMerging)
      .WillRepeatedly(Return(true));

  EXPECT_CALL(*external_view_embedder,
              BeginFrame(/*frame_size=*/SkISize(), /*context=*/nullptr,
                         /*device_pixel_ratio=*/2.0,
                         /*raster_thread_merger=*/_))
      .Times(2);
  EXPECT_CALL(*external_view_embedder, SubmitFrame).Times(2);
  EXPECT_CALL(*external_view_embedder, EndFrame(/*should_resubmit_frame=*/false,
                                                /*raster_thread_merger=*/_))
      .Times(2);

  rasterizer->Setup(std::move(surface));

  auto pipeline = std::make_shared<LayerTreePipeline>(/*depth=*/10);
  auto layer_tree = std::make_unique<LayerTree>(/*frame_size=*/SkISize(),
                                                /*device_pixel_ratio=*/2.0f);
  auto layer_tree_item = std::make_unique<LayerTreeItem>(
      std::move(layer_tree), CreateFinishedBuildRecorder());
  PipelineProduceResult result =
      pipeline->Produce().Complete(std::move(layer_tree_item));
  EXPECT_TRUE(result.success);
  auto no_discard = [](LayerTree&) { return false; };

  // The Draw() will respectively call BeginFrame(), SubmitFrame() and
  // EndFrame() one time.
  rasterizer->Draw(pipeline, no_discard);

  // The DrawLastLayerTree() will respectively call BeginFrame(), SubmitFrame()
  // and EndFrame() one more time, totally 2 times.
  rasterizer->DrawLastLayerTree(CreateFinishedBuildRecorder());
}

TEST(RasterizerTest, externalViewEmbedderDoesntEndFrameWhenNoSurfaceIsSet) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  MockDelegate delegate;
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);

  std::shared_ptr<MockExternalViewEmbedder> external_view_embedder =
      std::make_shared<MockExternalViewEmbedder>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);

  EXPECT_CALL(
      *external_view_embedder,
      EndFrame(/*should_resubmit_frame=*/false,
               /*raster_thread_merger=*/fml::RefPtr<fml::RasterThreadMerger>(
                   nullptr)))
      .Times(0);

  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<LayerTreePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(/*frame_size=*/SkISize(),
                                                  /*device_pixel_ratio=*/2.0f);
    auto layer_tree_item = std::make_unique<LayerTreeItem>(
        std::move(layer_tree), CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    auto no_discard = [](LayerTree&) { return false; };
    rasterizer->Draw(pipeline, no_discard);
    latch.Signal();
  });
  latch.Wait();
}

TEST(RasterizerTest, externalViewEmbedderDoesntEndFrameWhenNotUsedThisFrame) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  MockDelegate delegate;
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));

  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<MockSurface>();
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  std::shared_ptr<MockExternalViewEmbedder> external_view_embedder =
      std::make_shared<MockExternalViewEmbedder>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);
  rasterizer->Setup(std::move(surface));

  EXPECT_CALL(*external_view_embedder,
              BeginFrame(/*frame_size=*/SkISize(), /*context=*/nullptr,
                         /*device_pixel_ratio=*/2.0,
                         /*raster_thread_merger=*/_))
      .Times(0);
  EXPECT_CALL(
      *external_view_embedder,
      EndFrame(/*should_resubmit_frame=*/false,
               /*raster_thread_merger=*/fml::RefPtr<fml::RasterThreadMerger>(
                   nullptr)))
      .Times(0);

  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<LayerTreePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(/*frame_size=*/SkISize(),
                                                  /*device_pixel_ratio=*/2.0f);
    auto layer_tree_item = std::make_unique<LayerTreeItem>(
        std::move(layer_tree), CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    // Always discard the layer tree.
    auto discard_callback = [](LayerTree&) { return true; };
    RasterStatus status = rasterizer->Draw(pipeline, discard_callback);
    EXPECT_EQ(status, RasterStatus::kDiscarded);
    latch.Signal();
  });
  latch.Wait();
}

TEST(RasterizerTest, externalViewEmbedderDoesntEndFrameWhenPipelineIsEmpty) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  MockDelegate delegate;
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));

  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<MockSurface>();
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  std::shared_ptr<MockExternalViewEmbedder> external_view_embedder =
      std::make_shared<MockExternalViewEmbedder>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);
  rasterizer->Setup(std::move(surface));

  EXPECT_CALL(
      *external_view_embedder,
      EndFrame(/*should_resubmit_frame=*/false,
               /*raster_thread_merger=*/fml::RefPtr<fml::RasterThreadMerger>(
                   nullptr)))
      .Times(0);

  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<LayerTreePipeline>(/*depth=*/10);
    auto no_discard = [](LayerTree&) { return false; };
    RasterStatus status = rasterizer->Draw(pipeline, no_discard);
    EXPECT_EQ(status, RasterStatus::kFailed);
    latch.Signal();
  });
  latch.Wait();
}

TEST(RasterizerTest,
     drawWithGpuEnabledAndSurfaceAllowsDrawingWhenGpuDisabledDoesAcquireFrame) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  MockDelegate delegate;
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));

  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<MockSurface>();
  auto is_gpu_disabled_sync_switch =
      std::make_shared<const fml::SyncSwitch>(false);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;
  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/nullptr, /*framebuffer_info=*/framebuffer_info,
      /*submit_callback=*/[](const SurfaceFrame&, SkCanvas*) { return true; });
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(true));
  ON_CALL(delegate, GetIsGpuDisabledSyncSwitch())
      .WillByDefault(Return(is_gpu_disabled_sync_switch));
  EXPECT_CALL(delegate, GetIsGpuDisabledSyncSwitch()).Times(0);
  EXPECT_CALL(*surface, AcquireFrame(SkISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<LayerTreePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(/*frame_size=*/SkISize(),
                                                  /*device_pixel_ratio=*/2.0f);
    auto layer_tree_item = std::make_unique<LayerTreeItem>(
        std::move(layer_tree), CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    auto no_discard = [](LayerTree&) { return false; };
    rasterizer->Draw(pipeline, no_discard);
    latch.Signal();
  });
  latch.Wait();
}

TEST(
    RasterizerTest,
    drawWithGpuDisabledAndSurfaceAllowsDrawingWhenGpuDisabledDoesAcquireFrame) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  MockDelegate delegate;
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<MockSurface>();
  auto is_gpu_disabled_sync_switch =
      std::make_shared<const fml::SyncSwitch>(true);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;

  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/nullptr, /*framebuffer_info=*/framebuffer_info,
      /*submit_callback=*/[](const SurfaceFrame&, SkCanvas*) { return true; });
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(true));
  ON_CALL(delegate, GetIsGpuDisabledSyncSwitch())
      .WillByDefault(Return(is_gpu_disabled_sync_switch));
  EXPECT_CALL(delegate, GetIsGpuDisabledSyncSwitch()).Times(0);
  EXPECT_CALL(*surface, AcquireFrame(SkISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<LayerTreePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(/*frame_size=*/SkISize(),
                                                  /*device_pixel_ratio=*/2.0f);
    auto layer_tree_item = std::make_unique<LayerTreeItem>(
        std::move(layer_tree), CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    auto no_discard = [](LayerTree&) { return false; };
    RasterStatus status = rasterizer->Draw(pipeline, no_discard);
    EXPECT_EQ(status, RasterStatus::kSuccess);
    latch.Signal();
  });
  latch.Wait();
}

TEST(
    RasterizerTest,
    drawWithGpuEnabledAndSurfaceDisallowsDrawingWhenGpuDisabledDoesAcquireFrame) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  MockDelegate delegate;
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<MockSurface>();
  auto is_gpu_disabled_sync_switch =
      std::make_shared<const fml::SyncSwitch>(false);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;

  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/nullptr, /*framebuffer_info=*/framebuffer_info,
      /*submit_callback=*/[](const SurfaceFrame&, SkCanvas*) { return true; });
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(false));
  EXPECT_CALL(delegate, GetIsGpuDisabledSyncSwitch())
      .WillOnce(Return(is_gpu_disabled_sync_switch));
  EXPECT_CALL(*surface, AcquireFrame(SkISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<LayerTreePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(/*frame_size=*/SkISize(),
                                                  /*device_pixel_ratio=*/2.0f);
    auto layer_tree_item = std::make_unique<LayerTreeItem>(
        std::move(layer_tree), CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    auto no_discard = [](LayerTree&) { return false; };
    RasterStatus status = rasterizer->Draw(pipeline, no_discard);
    EXPECT_EQ(status, RasterStatus::kSuccess);
    latch.Signal();
  });
  latch.Wait();
}

TEST(
    RasterizerTest,
    drawWithGpuDisabledAndSurfaceDisallowsDrawingWhenGpuDisabledDoesntAcquireFrame) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  MockDelegate delegate;
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_)).Times(0);
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<MockSurface>();
  auto is_gpu_disabled_sync_switch =
      std::make_shared<const fml::SyncSwitch>(true);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;

  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/nullptr, /*framebuffer_info=*/framebuffer_info,
      /*submit_callback=*/[](const SurfaceFrame&, SkCanvas*) { return true; });
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(false));
  EXPECT_CALL(delegate, GetIsGpuDisabledSyncSwitch())
      .WillOnce(Return(is_gpu_disabled_sync_switch));
  EXPECT_CALL(*surface, AcquireFrame(SkISize())).Times(0);
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<LayerTreePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(/*frame_size=*/SkISize(),
                                                  /*device_pixel_ratio=*/2.0f);
    auto layer_tree_item = std::make_unique<LayerTreeItem>(
        std::move(layer_tree), CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    auto no_discard = [](LayerTree&) { return false; };
    RasterStatus status = rasterizer->Draw(pipeline, no_discard);
    EXPECT_EQ(status, RasterStatus::kDiscarded);
    latch.Signal();
  });
  latch.Wait();
}

TEST(RasterizerTest,
     drawLayerTreeWithCorrectFrameTimingWhenPipelineIsMoreAvailable) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  MockDelegate delegate;
  ON_CALL(delegate, GetTaskRunners()).WillByDefault(ReturnRef(task_runners));

  fml::AutoResetWaitableEvent latch;
  std::unique_ptr<Rasterizer> rasterizer;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    rasterizer = std::make_unique<Rasterizer>(delegate);
    latch.Signal();
  });
  latch.Wait();

  auto surface = std::make_unique<MockSurface>();
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled())
      .WillRepeatedly(Return(true));
  ON_CALL(*surface, AcquireFrame(SkISize()))
      .WillByDefault(::testing::Invoke([] {
        SurfaceFrame::FramebufferInfo framebuffer_info;
        framebuffer_info.supports_readback = true;
        return std::make_unique<SurfaceFrame>(
            /*surface=*/nullptr, framebuffer_info,
            /*submit_callback=*/[](const SurfaceFrame&, SkCanvas*) {
              return true;
            });
      }));
  ON_CALL(*surface, MakeRenderContextCurrent())
      .WillByDefault(::testing::Invoke(
          [] { return std::make_unique<GLContextDefaultResult>(true); }));

  fml::CountDownLatch count_down_latch(2);
  auto first_timestamp = fml::TimePoint::Now();
  auto second_timestamp = first_timestamp + fml::TimeDelta::FromMilliseconds(8);
  std::vector<fml::TimePoint> timestamps = {first_timestamp, second_timestamp};
  int frame_rasterized_count = 0;
  EXPECT_CALL(delegate, OnFrameRasterized(_))
      .Times(2)
      .WillRepeatedly([&](const FrameTiming& frame_timing) {
        EXPECT_EQ(timestamps[frame_rasterized_count],
                  frame_timing.Get(FrameTiming::kVsyncStart));
        EXPECT_EQ(timestamps[frame_rasterized_count],
                  frame_timing.Get(FrameTiming::kBuildStart));
        EXPECT_EQ(timestamps[frame_rasterized_count],
                  frame_timing.Get(FrameTiming::kBuildFinish));
        frame_rasterized_count++;
        count_down_latch.CountDown();
      });

  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    rasterizer->Setup(std::move(surface));
    auto pipeline = std::make_shared<LayerTreePipeline>(/*depth=*/10);
    for (int i = 0; i < 2; i++) {
      auto layer_tree =
          std::make_unique<LayerTree>(/*frame_size=*/SkISize(),
                                      /*device_pixel_ratio=*/2.0f);
      auto layer_tree_item = std::make_unique<LayerTreeItem>(
          std::move(layer_tree), CreateFinishedBuildRecorder(timestamps[i]));
      PipelineProduceResult result =
          pipeline->Produce().Complete(std::move(layer_tree_item));
      EXPECT_TRUE(result.success);
      EXPECT_EQ(result.is_first_item, i == 0);
    }
    auto no_discard = [](LayerTree&) { return false; };
    // Although we only call 'Rasterizer::Draw' once, it will be called twice
    // finally because there are two items in the pipeline.
    rasterizer->Draw(pipeline, no_discard);
  });
  count_down_latch.Wait();
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    rasterizer.reset();
    latch.Signal();
  });
  latch.Wait();
}

}  // namespace flutter
