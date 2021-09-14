#pragma once
#include "FrameSourceBase.h"


class GDIFrameSource : public FrameSourceBase {
public:
	GDIFrameSource() {};
	virtual ~GDIFrameSource();

	bool Initialize() override;

	ComPtr<ID3D11Texture2D> GetOutput() override;

	bool Update() override;

private:
	ComPtr<ID3D11DeviceContext4> _d3dDC;

	HWND _hwndSrc = NULL;
	ComPtr<ID3D11Texture2D> _output;

	HDC _hdcMem = NULL;
	HBITMAP _hbmMem = NULL;
};