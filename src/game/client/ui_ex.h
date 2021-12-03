#ifndef GAME_CLIENT_UI_EX_H
#define GAME_CLIENT_UI_EX_H

#include <base/system.h>
#include <engine/input.h>
#include <engine/kernel.h>
#include <game/client/ui.h>

class IInput;
class ITextRender;
class IKernel;
class IGraphics;

class CRenderTools;

class CUIEx
{
	CUI *m_pUI;
	IInput *m_pInput;
	ITextRender *m_pTextRender;
	IKernel *m_pKernel;
	IGraphics *m_pGraphics;
	CRenderTools *m_pRenderTools;

	IInput::CEvent *m_pInputEventsArray;
	int *m_pInputEventCount;

	bool m_MouseIsPress = false;
	bool m_HasSelection = false;

	int m_MousePressX = 0;
	int m_MousePressY = 0;
	int m_MouseCurX = 0;
	int m_MouseCurY = 0;
	bool m_MouseSlow;
	int m_CurSelStart = 0;
	int m_CurSelEnd = 0;
	void *m_pSelItem = nullptr;

	int m_CurCursor = 0;

protected:
	CUI *UI() const { return m_pUI; }
	IInput *Input() const { return m_pInput; }
	ITextRender *TextRender() const { return m_pTextRender; }
	IKernel *Kernel() const { return m_pKernel; }
	IGraphics *Graphics() const { return m_pGraphics; }
	CRenderTools *RenderTools() const { return m_pRenderTools; }

public:
	CUIEx();

	void Init(CUI *pUI, IKernel *pKernel, CRenderTools *pRenderTools, IInput::CEvent *pInputEventsArray, int *pInputEventCount);

	void ConvertMouseMove(float *pX, float *pY) const;
	void ResetMouseSlow() { m_MouseSlow = false; }

	float DoScrollbarV(const void *pID, const CUIRect *pRect, float Current);
	float DoScrollbarH(const void *pID, const CUIRect *pRect, float Current, const ColorRGBA *pColorInner = NULL);
	int DoEditBox(void *pID, const CUIRect *pRect, char *pStr, unsigned StrSize, float FontSize, float *Offset, bool Hidden = false, int Corners = CUI::CORNER_ALL, const char *pEmptyText = "");
};

#endif
