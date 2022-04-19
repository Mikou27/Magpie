#include "pch.h"
#include "GPUTimer.h"
#include "App.h"
#include "DeviceResources.h"
#include "Config.h"

using namespace std::chrono_literals;


void GPUTimer::OnBeginFrame() {
	auto now = std::chrono::high_resolution_clock::now();

	// 平滑过大的时间差
	_elapsedTime = std::min<std::chrono::nanoseconds>(now - _lastTimePoint, 100ms);
	_lastTimePoint = now;

	_totalTime += _elapsedTime;

	// 更新当前帧率
	++_framesThisSecond;
	++_frameCount;

	_fpsCounter += _elapsedTime;
	if (_fpsCounter >= 1s) {
		_framesPerSecond = _framesThisSecond;
		_framesThisSecond = 0;
		_fpsCounter %= 1s;
	}

	_UpdateGPUTimings();

	if (_curQueryIdx >= 0) {
		auto d3dDC = App::Get().GetDeviceResources().GetD3DDC();
		d3dDC->Begin(_queries[_curQueryIdx].disjoint.get());
		d3dDC->End(_queries[_curQueryIdx].start.get());
	}
}

void GPUTimer::StartProfiling(std::chrono::microseconds updateInterval, UINT passCount) {
	assert(passCount > 0);

	_curQueryIdx = 0;
	_updateProfilingTime = updateInterval;
	_profilingCounter = {};

	_queries[0].passes.resize(passCount);
	if (App::Get().GetConfig().IsDisableLowLatency()) {
		_queries[1].passes.resize(passCount);
	}
	_passesTimings.resize(passCount);
	_gpuTimings.passes.resize(passCount);
}

void GPUTimer::StopProfiling() {
	_curQueryIdx = -1;
	_updateProfilingTime = {};
	_profilingCounter = {};

	_queries = {};
	_passesTimings = {};
	_gpuTimings = {};
}

void GPUTimer::OnEndCapture() {
	if (_curQueryIdx < 0) {
		return;
	}

	App::Get().GetDeviceResources().GetD3DDC()->End(_queries[_curQueryIdx].capture.get());
}

void GPUTimer::OnEndPass(UINT idx) {
	if (_curQueryIdx < 0) {
		return;
	}

	App::Get().GetDeviceResources().GetD3DDC()->End(_queries[_curQueryIdx].passes[idx].get());
}

void GPUTimer::OnEndOverlay() {
	if (_curQueryIdx < 0) {
		return;
	}

	auto d3dDC = App::Get().GetDeviceResources().GetD3DDC();
	d3dDC->End(_queries[_curQueryIdx].overlay.get());
	d3dDC->End(_queries[_curQueryIdx].disjoint.get());
}

template<typename T>
static T GetQueryData(ID3D11DeviceContext3* d3dDC, ID3D11Query* query) {
	T data{};
	while (S_OK != d3dDC->GetData(query, &data, sizeof(data), 0)) {
		Sleep(0);
	}
	return data;
}

void GPUTimer::_UpdateGPUTimings() {
	if (_curQueryIdx < 0) {
		return;
	}

	if (App::Get().GetConfig().IsDisableLowLatency()) {
		_curQueryIdx = 1 - _curQueryIdx;
	}

	auto& curQueryInfo = _queries[_curQueryIdx];

	if (curQueryInfo.disjoint) {
		auto d3dDC = App::Get().GetDeviceResources().GetD3DDC();

		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData = 
			GetQueryData<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT>(d3dDC, curQueryInfo.disjoint.get());

		if (!disjointData.Disjoint) {
			const float toMS = 1000.0f / disjointData.Frequency;

			UINT64 startTimestamp = GetQueryData<UINT64>(d3dDC, curQueryInfo.start.get());

			UINT64 timestamp = GetQueryData<UINT64>(d3dDC, curQueryInfo.capture.get());
			_captureTimings.first += (timestamp - startTimestamp) * toMS;
			++_captureTimings.second;
			
			startTimestamp = timestamp;

			for (size_t i = 0; i < curQueryInfo.passes.size(); ++i) {
				timestamp = GetQueryData<UINT64>(d3dDC, curQueryInfo.passes[i].get());

				float t = (timestamp - startTimestamp) * toMS;
				if (t > 0.01) {
					_passesTimings[i].first += t;
					++_passesTimings[i].second;
				}
				startTimestamp = timestamp;
			}

			timestamp = GetQueryData<UINT64>(d3dDC, curQueryInfo.overlay.get());
			_overlayTimings.first += (timestamp - startTimestamp) * toMS;
			++_overlayTimings.second;
		} else {
			// 查询的值不可靠

#ifdef _DEBUG
			// 依然执行查询，否则调试层将发出警告
			GetQueryData<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT>(d3dDC, curQueryInfo.disjoint.get());
			GetQueryData<UINT64>(d3dDC, curQueryInfo.capture.get());
			for (auto& query : curQueryInfo.passes) {
				GetQueryData<UINT64>(d3dDC, query.get());
			}
			GetQueryData<UINT64>(d3dDC, curQueryInfo.overlay.get());
#endif // _DEBUG
		}

		_profilingCounter += _elapsedTime;
		if (_profilingCounter >= _updateProfilingTime) {
			// 更新渲染用时
			_gpuTimings.capture = _captureTimings.second == 0 ? 0.0f : _captureTimings.first / _captureTimings.second;
			for (UINT i = 0; i < _passesTimings.size(); ++i) {
				_gpuTimings.passes[i] = _passesTimings[i].second == 0 ?
					0.0f : _passesTimings[i].first / _passesTimings[i].second;
			}
			_gpuTimings.overlay = _overlayTimings.second == 0 ? 0.0f : _overlayTimings.first / _overlayTimings.second;

			_captureTimings = {};
			std::fill(_passesTimings.begin(), _passesTimings.end(), std::pair<float, UINT>());
			_overlayTimings = {};

			_profilingCounter %= _updateProfilingTime;
		}
	} else {
		auto d3dDevice = App::Get().GetDeviceResources().GetD3DDevice();

		D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
		d3dDevice->CreateQuery(&desc, curQueryInfo.disjoint.put());

		desc.Query = D3D11_QUERY_TIMESTAMP;
		d3dDevice->CreateQuery(&desc, curQueryInfo.start.put());
		d3dDevice->CreateQuery(&desc, curQueryInfo.capture.put());
		for (UINT j = 0; j < curQueryInfo.passes.size(); ++j) {
			d3dDevice->CreateQuery(&desc, curQueryInfo.passes[j].put());
		}
		d3dDevice->CreateQuery(&desc, curQueryInfo.overlay.put());
	}
}
