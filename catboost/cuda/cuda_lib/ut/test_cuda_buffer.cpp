#include <catboost/cuda/cuda_lib/gpu_memory_pool.h>
#include <library/unittest/registar.h>
#include <catboost/cuda/cuda_lib/cuda_manager.h>
#include <catboost/cuda/cuda_lib/cuda_buffer.h>
#include <catboost/cuda/cuda_lib/buffer_resharding.h>
#include <catboost/cuda/cuda_util/fill.h>
#include <catboost/cuda/cuda_util/cpu_random.h>

using namespace NCudaLib;

SIMPLE_UNIT_TEST_SUITE(TCudaBufferTest) {
    SIMPLE_UNIT_TEST(TestEmptyMappingIterator) {
        {
            auto& manager = GetCudaManager();
            manager.Start();
            {
                NCudaLib::TMirrorMapping mirrorMapping(0);
                for (auto dev : mirrorMapping.NonEmptyDevices()) {
                    Y_UNUSED(dev);
                    UNIT_ASSERT_C(false, TStringBuilder() << dev << " " << mirrorMapping.MemoryUsageAt(dev));
                }

                {
                    NCudaLib::TMirrorMapping mirrorMapping(1);
                    TVector<ui32> devs;
                    for (auto dev : mirrorMapping.NonEmptyDevices()) {
                        devs.push_back(dev);
                    }
                    for (ui32 i = 0; i < NCudaLib::GetCudaManager().GetDeviceCount(); ++i) {
                        UNIT_ASSERT(devs.at(i) == i);
                    }
                }
            }
            manager.Stop();
        }
    }

    SIMPLE_UNIT_TEST(TestDeviceIterator) {
        {
            auto& manager = GetCudaManager();
            manager.Start();
            ui32 devCount = manager.GetDeviceCount();
            {
                int offset = 0;
                for (ui32 dev : TDevicesList((1ULL << devCount) - 1)) {
                    UNIT_ASSERT_VALUES_EQUAL(dev, offset++);
                    UNIT_ASSERT_EQUAL(dev < devCount, true);
                }
            }
            {
                int offset = 1;
                for (ui32 dev : TDevicesList(((1ULL << devCount) - 1) & (~1))) {
                    UNIT_ASSERT_VALUES_EQUAL(dev, offset++);
                    UNIT_ASSERT_EQUAL(dev < devCount, true);
                    UNIT_ASSERT_EQUAL(dev > 0, true);
                }
            }
            {
                TVector<ui32> devs;
                for (ui32 dev : TMirrorMapping(10, 1).NonEmptyDevices()) {
                    devs.push_back(dev);
                }
                UNIT_ASSERT_EQUAL(devs.size(), devCount);
                for (ui32 dev = 0; dev < devCount; ++dev) {
                    UNIT_ASSERT_EQUAL(devs[dev], dev);
                }
            }
            {
                {
                    TVector<ui32> devs;
                    for (ui32 dev : TSingleMapping(0, 1, 1).NonEmptyDevices()) {
                        devs.push_back(dev);
                    }
                    UNIT_ASSERT_EQUAL(devs[0], 0);
                }
                if (devCount > 1) {
                    TVector<ui32> devs;
                    for (ui32 dev : TSingleMapping(1, 1, 1).NonEmptyDevices()) {
                        devs.push_back(dev);
                    }
                    UNIT_ASSERT_EQUAL(devs[0], 1);
                }
            }
            manager.Stop();
        }
    }

    SIMPLE_UNIT_TEST(SingleBufferTests) {
        {
            {
                auto& manager = NCudaLib::GetCudaManager();
                manager.Start();
                {
                    TCudaBuffer<float, TSingleMapping> buffer = TCudaBuffer<float, TSingleMapping>::Create(
                        TSingleMapping(0, 10, 4));
                    manager.WaitComplete();
                    UNIT_ASSERT_VALUES_EQUAL(buffer.GetMapping().MemoryUsageAt(0), 40ULL);
                    manager.WaitComplete();
                    UNIT_ASSERT_VALUES_EQUAL(buffer.GetMapping().MemoryUsageAt(1), 0);
                    manager.WaitComplete();
                    UNIT_ASSERT_VALUES_EQUAL(buffer.GetMapping().MemorySize(TSlice(0, 4)), 16);
                    manager.WaitComplete();
                }
                manager.Stop();
            }
        }
    }

    SIMPLE_UNIT_TEST(MirrorBufferTests) {
        {
            auto& manager = NCudaLib::GetCudaManager();
            manager.Start();
            {
                const ui32 objectSize = 4;
                const ui32 objectCount = 10;
                auto buffer = TCudaBuffer<float, TMirrorMapping>::Create(TMirrorMapping(objectCount, objectSize));
                UNIT_ASSERT_VALUES_EQUAL(buffer.GetMapping().MemoryUsageAt(0), objectSize * objectCount);

                UNIT_ASSERT_VALUES_EQUAL(buffer.GetMapping().MemoryUsageAt(1), objectCount * objectSize);

                UNIT_ASSERT_VALUES_EQUAL(buffer.GetMapping().MemorySize(TSlice(0, 4)), 4 * objectSize);
                UNIT_ASSERT_VALUES_EQUAL(buffer.GetMapping().DeviceSlice(1), TSlice(0, objectCount));

                TVector<float> tmp;
                for (ui32 i = 0; i < objectCount * objectSize; ++i) {
                    tmp.push_back((float)i);
                }

                buffer.CreateWriter(tmp).Write();
                TVector<float> tmp2;
                buffer.CreateReader().Read(tmp2);
                UNIT_ASSERT_VALUES_EQUAL(tmp2.size(), objectCount * objectSize);
                for (ui32 i = 0; i < objectCount * objectSize; ++i) {
                    UNIT_ASSERT_VALUES_EQUAL(tmp[i], tmp2[i]);
                }
            }
            manager.Stop();
        }
    }

    SIMPLE_UNIT_TEST(SliceBufferTests) {
        {
            auto& manager = NCudaLib::GetCudaManager();
            manager.Start();
            {
                const ui32 objectSize = 4;
                const ui32 objectCount = 1024;
                auto buffer = TCudaBuffer<float, TMirrorMapping>::Create(TMirrorMapping(objectCount, objectSize));

                TVector<float> tmp;
                for (ui32 i = 0; i < objectCount * objectSize; ++i) {
                    tmp.push_back((float)i);
                }

                buffer.CreateWriter(tmp).Write();

                const auto& constRef = buffer;

                TVector<float> tmp1;
                TVector<float> tmp2;
                TVector<float> tmp3;
                auto fullSliceBuffer = buffer.SliceView(buffer.GetMapping().GetObjectsSlice());
                auto sliceBuffer = buffer.SliceView(TSlice(3, 5));
                auto constSliceBuffer = constRef.SliceView(TSlice(3, 5));
                fullSliceBuffer.Read(tmp1);
                sliceBuffer.Read(tmp2);
                constSliceBuffer.Read(tmp3);
                for (ui32 i = 0; i < tmp.size(); ++i) {
                    UNIT_ASSERT_DOUBLES_EQUAL(tmp[i], tmp1[i], 1e-20);
                }
                UNIT_ASSERT_VALUES_EQUAL(tmp2.size(), 2 * objectSize);
                for (ui32 i = 3 * objectSize; i < 5 * objectSize; ++i) {
                    UNIT_ASSERT_VALUES_EQUAL(tmp[i], tmp2[i - 3 * objectSize]);
                    UNIT_ASSERT_VALUES_EQUAL(tmp[i], tmp3[i - 3 * objectSize]);
                    tmp2[i - 3 * objectSize] = -i;
                    tmp[i] = -i;
                }
                sliceBuffer.Write(tmp2);
                sliceBuffer.Read(tmp3);
                for (ui32 i = 0; i < tmp2.size(); ++i) {
                    UNIT_ASSERT_VALUES_EQUAL(tmp2[i], tmp3[i]);
                }

                buffer.Read(tmp3);
                for (ui32 i = 0; i < tmp.size(); ++i) {
                    UNIT_ASSERT_DOUBLES_EQUAL(tmp[i], tmp3[i], 1e-20);
                }
            }
            manager.Stop();
        }
    }

    SIMPLE_UNIT_TEST(SeveralSliceBufferTests) {
        {
            auto& manager = NCudaLib::GetCudaManager();
            manager.Start();
            {
                const ui32 objectCount = 4096;
                auto buffer = TStripeBuffer<float>::Create(TStripeMapping::SplitBetweenDevices(objectCount));

                TVector<float> tmp;
                for (ui32 i = 0; i < objectCount; ++i) {
                    tmp.push_back((float)i);
                }

                buffer.CreateWriter(tmp).Write();

                const auto& constRef = buffer;

                auto half = constRef.SliceView(TSlice(0, objectCount / 2));
                auto a = constRef.SliceView(TSlice(0, objectCount / 2));
                auto quad = a.SliceView(TSlice(0, objectCount / 4));
                const TSlice quad2Slice = TSlice(objectCount / 4, objectCount / 2);
                auto quad2 = constRef.SliceView(TSlice(0, objectCount / 2)).SliceView(quad2Slice);

                TVector<float> tmp1;
                TVector<float> tmp2;
                TVector<float> tmp3;

                half.Read(tmp1);
                quad.Read(tmp2);
                quad2.Read(tmp3);

                UNIT_ASSERT_VALUES_EQUAL(tmp1.size(), objectCount / 2);
                UNIT_ASSERT_VALUES_EQUAL(tmp2.size(), objectCount / 4);
                UNIT_ASSERT_VALUES_EQUAL(tmp3.size(), quad2Slice.Size());

                for (ui32 i = 0; i < objectCount / 2; ++i) {
                    UNIT_ASSERT_DOUBLES_EQUAL(tmp[i], tmp1[i], 1e-20);
                    if (i < objectCount / 4) {
                        UNIT_ASSERT_DOUBLES_EQUAL(tmp[i], tmp2[i], 1e-20);
                        UNIT_ASSERT_DOUBLES_EQUAL(tmp[i + objectCount / 4], tmp3[i], 1e-20);
                    }
                }
            }
            manager.Stop();
        }
    }

    SIMPLE_UNIT_TEST(StripeBufferTests) {
        auto& manager = NCudaLib::GetCudaManager();
        manager.Start();
        {
            auto devCount = manager.GetDeviceCount();
            const ui32 count = 256;
            const ui32 objectSize = 7;
            TStripeMapping mapping = TStripeMapping::SplitBetweenDevices(count, objectSize);

            auto buffer = TCudaBuffer<ui32, TStripeMapping>::Create(mapping);
            UNIT_ASSERT_VALUES_EQUAL(buffer.GetMapping().MemoryUsageAt(0), objectSize * ((count + devCount - 1) / devCount));
            UNIT_ASSERT_VALUES_EQUAL(buffer.GetMapping().MemorySize(TSlice(0, 5)), 5 * objectSize);

            if (devCount > 1) {
                UNIT_ASSERT_VALUES_EQUAL(buffer.GetMapping().DeviceSlice(0).Right, buffer.GetMapping().DeviceSlice(1).Left);
            }

            TVector<ui32> tmp;
            for (ui32 i = 0; i < count * objectSize; ++i) {
                tmp.push_back(i);
            }

            buffer.CreateWriter(tmp).Write();
            TVector<ui32> tmp2;
            buffer.CreateReader().Read(tmp2);

            UNIT_ASSERT_VALUES_EQUAL(tmp2.size(), count * objectSize);
            for (ui32 i = 0; i < objectSize * count; ++i) {
                UNIT_ASSERT_VALUES_EQUAL(tmp[i], tmp2[i]);
            }

            TSlice partSlice = {(count / 10), (count * 9 / 10)};
            for (ui32 i = partSlice.Left * objectSize; i < partSlice.Right * objectSize; ++i) {
                tmp[i] = 100500 + i;
            }
            {
                buffer.CreateWriter(tmp)
                    .SetWriteSlice(partSlice)
                    .Write();
            }
            {
                TVector<ui32> tmp3;
                buffer.CreateReader().SetReadSlice(partSlice).Read(tmp3);
                UNIT_ASSERT_VALUES_EQUAL(tmp3.size(), buffer.GetMapping().MemorySize(partSlice));

                for (ui32 i = partSlice.Left * objectSize; i < partSlice.Right * objectSize; ++i) {
                    UNIT_ASSERT_VALUES_EQUAL(tmp[i], tmp3[i - partSlice.Left * objectSize]);
                }
            }
        }
        manager.Stop();
    }

    SIMPLE_UNIT_TEST(MultiColumnBufferTests) {
        auto& manager = NCudaLib::GetCudaManager();
        manager.Start();
        {
            const ui32 count = 123495;

            TStripeMapping mapping = TStripeMapping(TStripeMapping::SplitBetweenDevices(count));
            auto buffer = TCudaBuffer<float, TStripeMapping>::Create(mapping, 2);

            TVector<float> target;
            TVector<float> weight;
            for (ui32 i = 0; i < count; ++i) {
                target.push_back(i * 1.0f);
                weight.push_back(1.0f / i);
            }

            buffer.CreateWriter(target).SetColumnWriteSlice(TSlice(0)).Write();
            buffer.CreateWriter(weight).SetColumnWriteSlice(TSlice(1)).Write();

            TVector<float> tmp2;
            TVector<float> target2;
            TVector<float> weights2;
            buffer.CreateReader().Read(tmp2);
            buffer.CreateReader().SetColumnReadSlice(TSlice(0)).Read(target2);
            buffer.CreateReader().SetColumnReadSlice(TSlice(1)).Read(weights2);

            UNIT_ASSERT_VALUES_EQUAL(tmp2.size(), count * 2);
            for (ui32 i = 0; i < count; ++i) {
                UNIT_ASSERT_VALUES_EQUAL(target[i], tmp2[i]);
                UNIT_ASSERT_VALUES_EQUAL(target[i], target2[i]);
            }
            for (ui32 i = 0; i < count; ++i) {
                UNIT_ASSERT_VALUES_EQUAL(weight[i], tmp2[target.size() + i]);
                UNIT_ASSERT_VALUES_EQUAL(weight[i], weights2[i]);
            }
        }
        manager.Stop();
    }

    SIMPLE_UNIT_TEST(CopyTest) {
        auto& manager = NCudaLib::GetCudaManager();
        manager.Start();
        {
            const ui64 count = 1 << 14;
            const ui64 objectSize = 7;
            TStripeMapping mapping = TStripeMapping::SplitBetweenDevices(count, objectSize);

            auto buffer = TCudaBuffer<ui64, TStripeMapping>::Create(mapping);

            TVector<ui64> tmp;
            for (ui64 i = 0; i < count * objectSize; ++i) {
                tmp.push_back(i % 10050);
            }

            buffer.CreateWriter(tmp).Write();

            auto copyBuffer = TCudaBuffer<ui64, TStripeMapping>::CopyMapping(buffer);
            FillBuffer(copyBuffer, static_cast<ui64>(1));
            copyBuffer.Copy(buffer);

            TVector<ui64> tmp2;
            copyBuffer.CreateReader().Read(tmp2);

            UNIT_ASSERT_VALUES_EQUAL(tmp2.size(), count * objectSize);
            for (ui64 i = 0; i < objectSize * count; ++i) {
                UNIT_ASSERT_VALUES_EQUAL(tmp[i], tmp2[i]);
            }
        }
        manager.Stop();
    }

    template <class T, class TBuffer>
    inline void Validate(const TVector<T>& ref, const TBuffer& buffer) {
        TVector<T> target;
        buffer.Read(target);
        UNIT_ASSERT_VALUES_EQUAL(ref.size(), target.size());
        for (ui64 i = 0; i < ref.size(); ++i) {
            Y_ASSERT(ref[i] == target[i]);
            UNIT_ASSERT_VALUES_EQUAL(ref[i], target[i]);
        }
    }

    static TStripeMapping SplitBetweenDevicesRandom(TRandom & rng,
                                                    ui64 objectCount,
                                                    ui64 objectSize = 1) {
        const ui64 devCount = GetCudaManager().GetDeviceCount();
        TVector<TSlice> slices(devCount);
        const ui64 objectPerDevice = ((objectCount + devCount - 1) / devCount) + rng.NextUniformL() % (objectCount / devCount / 10);

        ui64 total = 0;

        for (ui32 i = 0; i < devCount; ++i) {
            const ui64 devSize = Min(objectCount - total, objectPerDevice);
            slices[i] = TSlice(total, total + devSize);
            total += devSize;
        }
        return TStripeMapping(std::move(slices), objectSize);
    }

    inline void RunReshardTest(NCudaLib::TCudaManager & manager) {
        TRandom rng(0);
        if (manager.GetDeviceCount() > 1) {
            const ui64 count = 1 << 25;
            const ui64 objectSize = 3;

            auto singleMapping = TSingleMapping(1, count, objectSize);
            auto singleMappingOtherDev = TSingleMapping(0, count, objectSize);
            auto mirrorMapping = TMirrorMapping(count, objectSize);
            auto stripeMapping = TStripeMapping::SplitBetweenDevices(count, objectSize);
            auto anotherStripeMapping = SplitBetweenDevicesRandom(rng, count, objectSize);

            TVector<float> reference;
            for (ui64 i = 0; i < count * objectSize; ++i) {
                reference.push_back(((i * count + objectSize) % 10050) * 1.0f);
            }

            auto bufferSingle = TSingleBuffer<float>::Create(singleMapping);
            auto bufferSingleOtherDev = TSingleBuffer<float>::Create(singleMappingOtherDev);
            auto bufferStripe = TStripeBuffer<float>::Create(stripeMapping);
            auto bufferMirror = TMirrorBuffer<float>::Create(mirrorMapping);
            auto bufferStripeRandom = TStripeBuffer<float>::Create(anotherStripeMapping);

            bufferSingle.CreateWriter(reference).Write();

            //single -> mirror -> stripe -> stripe -> single
            {
                FillBuffer(bufferMirror, 1.0f);
                Reshard(bufferSingle, bufferMirror);
                Validate(reference, bufferMirror);
                for (ui32 i = 0; i < NCudaLib::GetCudaManager().GetDeviceCount(); ++i) {
                    Validate(reference, bufferMirror.DeviceView(i));
                }

                FillBuffer(bufferStripe, 1.0f);
                Reshard(bufferMirror, bufferStripe);
                Validate(reference, bufferStripe);
                //
                FillBuffer(bufferStripeRandom, 1.0f);
                Reshard(bufferStripe, bufferStripeRandom);
                Validate(reference, bufferStripeRandom);
                //
                FillBuffer(bufferSingleOtherDev, 1.0f);
                Reshard(bufferStripeRandom, bufferSingleOtherDev);
                Validate(reference, bufferSingleOtherDev);
            }

            //single -> single -> stripe -> mirror
            {
                FillBuffer(bufferSingleOtherDev, 1.0f);
                Reshard(bufferSingle, bufferSingleOtherDev);
                Validate(reference, bufferSingleOtherDev);

                FillBuffer(bufferStripe, 1.0f);
                Reshard(bufferSingleOtherDev, bufferStripe);
                Validate(reference, bufferStripe);

                FillBuffer(bufferMirror, 1.0f);
                Reshard(bufferStripe, bufferMirror);
                for (i32 i = NCudaLib::GetCudaManager().GetDeviceCount() - 1; i >= 0; --i) {
                    Validate(reference, bufferMirror.DeviceView(i));
                }
                Validate(reference, bufferMirror);
            }
        }
    }

    SIMPLE_UNIT_TEST(ReshardingTest) {
        auto& manager = NCudaLib::GetCudaManager();
        manager.Start();
        RunReshardTest(manager);
        manager.Stop();
    }

    //
}