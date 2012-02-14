/******************************************************************************
/ sws_wnd.cpp
/
/ Copyright (c) 2011 Tim Payne (SWS), Jeffos
/ http://www.standingwaterstudios.com/reaper
/
/ Permission is hereby granted, free of charge, to any person obtaining a copy
/ of this software and associated documentation files (the "Software"), to deal
/ in the Software without restriction, including without limitation the rights to
/ use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
/ of the Software, and to permit persons to whom the Software is furnished to
/ do so, subject to the following conditions:
/ 
/ The above copyright notice and this permission notice shall be included in all
/ copies or substantial portions of the Software.
/ 
/ THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
/ EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
/ OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
/ NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
/ HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
/ WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
/ FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
/ OTHER DEALINGS IN THE SOFTWARE.
/
******************************************************************************/


/* From askjf.com:
    * It's often the easiest to build your application out of plain dialog (DialogBoxParam() or CreateDialogParam())
    * Use Get/SetWindowLong(hwnd,GWL_USERDATA) (or GWLP_USERDATA for x64) for "this" pointers, if an object owns the dialog. Set it in WM_INITDIALOG, and get it for other messages.
    * On the same token, when the owner object has a dialog, it's good to do the following:
          o The object's constructor sets m_hwnd to 0.
          o The object's destructor does if (m_hwnd) DestroyWindow(m_hwnd);
          o WM_INITDIALOG sets this->m_hwnd to hwndDlg
          o WM_DESTROY clears this->m_hwnd to 0
*/

#include "stdafx.h"

#define CELL_EDIT_TIMER		0x1000
#define CELL_EDIT_TIMEOUT	50
#define TOOLTIP_TIMER		0x1001
#define TOOLTIP_TIMEOUT		350


SWS_DockWnd::SWS_DockWnd(int iResource, const char* cWndTitle, const char* cId, int iDockOrder, int iCmdID)
:m_hwnd(NULL), m_iResource(iResource), m_cWndTitle(cWndTitle), m_cId(cId), m_iDockOrder(iDockOrder), m_bUserClosed(false), m_iCmdID(iCmdID), m_bLoadingState(false)
{
	if (screenset_registerNew) // v4
		screenset_registerNew((char*)cId, screensetCallback, this);
	else
		screenset_register((char*)cId, (void*)screensetCallbackOld, this);

	memset(&m_state, 0, sizeof(SWS_DockWnd_State));
	*m_tooltip = '\0';

	m_ar.translateAccel = keyHandler;
	m_ar.isLocal = true;
	m_ar.user = this;
	plugin_register("accelerator", &m_ar);
}

// Init() must be called from the constructor of *every* derived class.  Unfortunately,
// you can't just call Init() from the DockWnd/base class, because the vTable isn't
// setup yet.
void SWS_DockWnd::Init()
{
	// Restore state
	// First, get the # of bytes
	int iLen = sizeof(SWS_DockWnd_State) + SaveView(NULL, 0);
	char* cState = new char[iLen];
	memset(cState, 0, iLen);
	// Then load the state from the INI
	GetPrivateProfileStruct(SWS_INI, m_cId, cState, iLen, get_ini_file());
	LoadState(cState, iLen);
	delete [] cState;
}

SWS_DockWnd::~SWS_DockWnd()
{
	plugin_register("-accelerator", &m_ar);
}

void SWS_DockWnd::Show(bool bToggle, bool bActivate)
{
	if (!IsWindow(m_hwnd))
	{
		CreateDialogParam(g_hInst, MAKEINTRESOURCE(m_iResource), g_hwndParent, SWS_DockWnd::sWndProc, (LPARAM)this);
		if (IsDocked() && bActivate)
			DockWindowActivate(m_hwnd);
#ifndef _WIN32
		// TODO see if DockWindowRefresh works here
		InvalidateRect(m_hwnd, NULL, TRUE);
#endif
	}
	else if (!IsWindowVisible(m_hwnd) || (bActivate && !bToggle))
	{
		if ((m_state.state & 2))
			DockWindowActivate(m_hwnd);
		else
			ShowWindow(m_hwnd, SW_SHOW);
		SetFocus(m_hwnd);
	}
	else if (bToggle)// If already visible close the window
		SendMessage(m_hwnd, WM_COMMAND, IDCANCEL, 0);
}

bool SWS_DockWnd::IsActive(bool bWantEdit)
{
	if (!IsValidWindow())
		return false;

	for (int i = 0; i < m_pLists.GetSize(); i++)
		if (m_pLists.Get(i)->IsActive(bWantEdit))
			return true;

	return GetFocus() == m_hwnd || IsChild(m_hwnd, GetFocus());
}

INT_PTR WINAPI SWS_DockWnd::sWndProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	SWS_DockWnd* pObj = (SWS_DockWnd*)GetWindowLongPtr(hwndDlg, GWLP_USERDATA);
	if (!pObj && uMsg == WM_INITDIALOG)
	{
		SetWindowLongPtr(hwndDlg, GWLP_USERDATA, lParam);
		pObj = (SWS_DockWnd*)lParam;
		pObj->m_hwnd = hwndDlg;
	}
	if (pObj)
		return pObj->WndProc(uMsg, wParam, lParam);
	else
		return 0;
}

INT_PTR SWS_DockWnd::WndProc(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
		case WM_INITDIALOG:
		{
	        m_resize.init(m_hwnd);

			// Call derived class initialization
			OnInitDlg();

			if ((m_state.state & 2))
			{
				if (DockWindowAddEx)
					DockWindowAddEx(m_hwnd, (char*)m_cWndTitle, (char*)m_cId, true);
				else
					DockWindowAdd(m_hwnd, (char*)m_cWndTitle, m_iDockOrder, true); 
			}
			else
			{
				EnsureNotCompletelyOffscreen(&m_state.r);
				if (m_state.r.left || m_state.r.top || m_state.r.right || m_state.r.bottom)
					SetWindowPos(m_hwnd, NULL, m_state.r.left, m_state.r.top, m_state.r.right-m_state.r.left, m_state.r.bottom-m_state.r.top, SWP_NOZORDER);
				if (AttachWindowTopmostButton) // v4 only
				{
					AttachWindowTopmostButton(m_hwnd);
					AttachWindowResizeGrip(m_hwnd);
				}
				ShowWindow(m_hwnd, SW_SHOW);
			}

			break;
		}
		case WM_TIMER:
			if (wParam == CELL_EDIT_TIMER)
			{
				for (int i = 0; i < m_pLists.GetSize(); i++)
					if (m_pLists.Get(i)->GetEditingItem() != -1)
						return m_pLists.Get(i)->OnEditingTimer();
			}
			else if (wParam == TOOLTIP_TIMER)
			{
				KillTimer(m_hwnd, wParam);

				POINT p; GetCursorPos(&p);
				ScreenToClient(m_hwnd,&p);
				RECT r; GetClientRect(m_hwnd,&r);
				char buf[TOOLTIP_MAX_LEN] = "";
				if (PtInRect(&r,p))
					if (!m_parentVwnd.GetToolTipString(p.x,p.y,buf,sizeof(buf)))
						if (!GetToolTipString(p.x,p.y,buf,sizeof(buf)))
							*buf='\0';

				if (strcmp(buf, m_tooltip))
				{
					m_tooltip_pt = p;
					lstrcpyn(m_tooltip,buf,sizeof(m_tooltip));
					InvalidateRect(m_hwnd,NULL,FALSE);
				}
			}
			else
				OnTimer(wParam);
			break;
		case WM_NOTIFY:
		{
			NMHDR* hdr = (NMHDR*)lParam;
			for (int i = 0; i < m_pLists.GetSize(); i++)
				if (hdr->hwndFrom == m_pLists.Get(i)->GetHWND())
					return m_pLists.Get(i)->OnNotify(wParam, lParam);

			return OnNotify(wParam, lParam);

			/* for future coloring	if (s->hdr.code == LVN_ITEMCHANGING)
			{
				SetWindowLong(hwndDlg, DWL_MSGRESULT, TRUE);
				return TRUE;
			} 
			break;*/
		}
		case WM_CONTEXTMENU:
		{
			KillTooltip(true);
			int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
			for (int i = 0; i < m_pLists.GetSize(); i++)
			{
				m_pLists.Get(i)->EditListItemEnd(true);
				// Are we over the column header?
				if (m_pLists.Get(i)->DoColumnMenu(x, y))
					return 0;
			}
			
			// SWS issue 373 - removed code from here that removed all but one selection on right click on OSX

			bool wantDefaultItems = true;
			HMENU hMenu = OnContextMenu(x, y, &wantDefaultItems);
			if (wantDefaultItems)
			{
				if (!hMenu)
					hMenu = CreatePopupMenu();
				else
					AddToMenu(hMenu, SWS_SEPARATOR, 0);

				// Add std menu items
				char str[100];
				if (_snprintf(str, 100, "Dock %s in Docker", m_cWndTitle) > 0)
					AddToMenu(hMenu, str, DOCK_MSG);

				// Check dock state
				if ((m_state.state & 2))
					CheckMenuItem(hMenu, DOCK_MSG, MF_BYCOMMAND | MF_CHECKED);
				AddToMenu(hMenu, "Close Window", IDCANCEL);
			}
			
			if (hMenu)
			{
				if (x == -1 || y == -1)
				{
					RECT r;
					GetWindowRect(m_hwnd, &r);
					x = r.left;
					y = r.top;
				}
				kbd_reprocessMenu(hMenu, NULL);
				TrackPopupMenu(hMenu, 0, x, y, 0, m_hwnd, NULL);
				DestroyMenu(hMenu);
			}
			break;
		}
		case WM_COMMAND:
			OnCommand(wParam, lParam);
			switch (wParam)
			{
				case DOCK_MSG:
					ToggleDocking();
					break;
				case IDCANCEL:
				case IDOK:
					m_bUserClosed = true;
					DestroyWindow(m_hwnd);
					break;
			}
			break;
		case WM_SIZE:
			if (wParam != SIZE_MINIMIZED)
			{
				static bool bRecurseCheck = false;
				if (!bRecurseCheck)
				{
					bRecurseCheck = true;
					KillTooltip();
					OnResize();
					m_resize.onResize();
					bRecurseCheck = false;
				}
			}
			break;
		// define a min size (+ fix flickering when of docked views)
		case WM_GETMINMAXINFO:
			if (lParam)
			{
				int w, h;
				GetMinSize(&w, &h);
				LPMINMAXINFO l = (LPMINMAXINFO)lParam;
				l->ptMinTrackSize.x = w;
				l->ptMinTrackSize.y = h;
			}
			break;
		case WM_DROPFILES:
			OnDroppedFiles((HDROP)wParam);
			break;
		case WM_DESTROY:
		{
			KillTimer(m_hwnd, CELL_EDIT_TIMER);
			KillTooltip();

			OnDestroy();

			m_parentVwnd.RemoveAllChildren(false);
			m_parentVwnd.SetRealParent(NULL);

			for (int i = 0; i < m_pLists.GetSize(); i++)
				m_pLists.Get(i)->OnDestroy();

			char cState[256];
			int iLen = SaveState(cState, 256);
			WritePrivateProfileStruct(SWS_INI, m_cId, cState, iLen, get_ini_file());
			m_bUserClosed = false;
			
			DockWindowRemove(m_hwnd); // Always safe to call even if the window isn't docked
		}
#ifdef _WIN32
			break;
		case WM_NCDESTROY:
#endif
			m_pLists.Empty(true);
			m_hwnd = NULL;
			RefreshToolbar(m_iCmdID);
			break;
		case WM_PAINT:
			OnPaint();
			if (m_parentVwnd.GetNumChildren())
			{
				int xo, yo; RECT r;
				GetClientRect(m_hwnd,&r);		
				m_parentVwnd.SetPosition(&r);
				m_vwnd_painter.PaintBegin(m_hwnd, WDL_STYLE_GetSysColor(COLOR_WINDOW));
				if (LICE_IBitmap* bm = m_vwnd_painter.GetBuffer(&xo, &yo))
				{
					bm->resize(r.right-r.left,r.bottom-r.top);
					int x=0; while (WDL_VWnd* w = m_parentVwnd.EnumChildren(x++)) w->SetVisible(false);
					int h=0; DrawControls(bm, &r, &h);
					m_vwnd_painter.PaintVirtWnd(&m_parentVwnd);
					if (*m_tooltip)
					{
						if (!(*(int*)GetConfigVar("tooltips")&2)) // obeys the "Tooltip for UI elements" pref
						{
							POINT p = { m_tooltip_pt.x + xo, m_tooltip_pt.y + yo };
							RECT rr = { r.left+xo,r.top+yo,r.right+xo,r.bottom+yo };
							if (h>0) rr.bottom = h+yo; //JFB WIP.. make sure some tooltips are not hidden by MFCs..
							DrawTooltipForPoint(bm,p,&rr,m_tooltip);
						}
						Help_Set(m_tooltip, true);
					}
				}
				m_vwnd_painter.PaintEnd();
			}
			break;
		case WM_LBUTTONDBLCLK:
			if (!OnMouseDblClick(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)) &&
				m_parentVwnd.OnMouseDown(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)) > 0)
			{
				m_parentVwnd.OnMouseUp(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam));
			}
			break;
		case WM_LBUTTONDOWN:
			KillTooltip(true);
			SetFocus(m_hwnd); 
			if (OnMouseDown(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)) > 0 ||
				m_parentVwnd.OnMouseDown(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)) > 0)
			{
				SetCapture(m_hwnd);
			}
			break;
		case WM_LBUTTONUP:
			if (GetCapture() == m_hwnd)
			{
				if (!OnMouseUp(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)))
					m_parentVwnd.OnMouseUp(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam));
				ReleaseCapture();
			}
			KillTooltip(true);
			break;
		case WM_MOUSEMOVE:
			m_parentVwnd.OnMouseMove(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)); // no capture test (hover, etc..)
			if (GetCapture() == m_hwnd)
			{
				if (!OnMouseMove(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)))
				{
					POINT pt = {GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
					for (int i=0; i < m_pLists.GetSize(); i++)
					{
						RECT r;	GetWindowRect(m_pLists.Get(i)->GetHWND(), &r);
						ScreenToClient(m_hwnd, (LPPOINT)&r);
						ScreenToClient(m_hwnd, ((LPPOINT)&r)+1);
						if (PtInRect(&r,pt)) {
							m_pLists.Get(i)->OnDrag();
							break;
						}
					}
				}
			}
			else
			{
				KillTooltip(true);
				SetTimer(m_hwnd, TOOLTIP_TIMER, TOOLTIP_TIMEOUT, NULL);
			}
			break;
#ifdef _WIN32
		case WM_CTLCOLOREDIT:
			return (INT_PTR)OnColorEdit((HWND)lParam, (HDC)wParam);
#endif
		default:
			return OnUnhandledMsg(uMsg, wParam, lParam);
	}
	return 0;
}

void SWS_DockWnd::ToggleDocking()
{
	if (!IsDocked())
		GetWindowRect(m_hwnd, &m_state.r);

	DestroyWindow(m_hwnd);
	m_state.state ^= 2;
	Show(false, true);
}

// v3 screenset support (delete for v4)
LRESULT SWS_DockWnd::screensetCallbackOld(int action, char *id, void *param, int param2)
{
	return screensetCallback(action, id, param, NULL, param2);
}

// v4 screenset support
LRESULT SWS_DockWnd::screensetCallback(int action, char *id, void *param, void *actionParm, int actionParmSize)
{
	SWS_DockWnd* pObj = (SWS_DockWnd*)param;
	switch(action)
	{
	case SCREENSET_ACTION_GETHWND:
		return (LRESULT)pObj->m_hwnd;
	case SCREENSET_ACTION_IS_DOCKED:
		return (LRESULT)pObj->IsDocked();
	// *** V3 only (remove for v4)
	case SCREENSET_ACTION_SHOW:
		if (IsWindow(pObj->m_hwnd))
			DestroyWindow(pObj->m_hwnd);
		if (actionParmSize) pObj->m_state.state |= 2; else pObj->m_state.state &= ~2;
		pObj->Show(false, true);
		break;
	case SCREENSET_ACTION_CLOSE:
		if (IsWindow(pObj->m_hwnd))
		{
			pObj->m_bUserClosed = true;
			DestroyWindow(pObj->m_hwnd);
		}
		break;
	case SCREENSET_ACTION_SWITCH_DOCK:
		if (IsWindow(pObj->m_hwnd))
			pObj->ToggleDocking();
		break;
	case SCREENSET_ACTION_LOAD_STATE:
		pObj->LoadState((char*)actionParm, actionParmSize);
		break;
	case SCREENSET_ACTION_SAVE_STATE:
		return pObj->SaveState((char*)actionParm, actionParmSize);
	}
	return 0;
}

int SWS_DockWnd::keyHandler(MSG* msg, accelerator_register_t* ctx)
{
	SWS_DockWnd* p = (SWS_DockWnd*)ctx->user;
	if (p && p->IsActive(true))
	{
		// First check for editing keys
		SWS_ListView* pLV = NULL;
		for (int i = 0; i < p->m_pLists.GetSize(); i++)
		{
			pLV = p->m_pLists.Get(i);
			if (pLV->IsActive(true))
			{
				int iRet = pLV->EditingKeyHandler(msg);
				if (iRet)
					return iRet;
				break;
			}
		}

		// Check the derived class key handler next in case they want to override anything
		int iKeys = SWS_GetModifiers();
		int iRet = p->OnKey(msg, iKeys);
		if (iRet)
			return iRet;

		// Key wasn't handled by the DockWnd, check for keys to send to LV
		if (pLV)
		{
			iRet = pLV->LVKeyHandler(msg, iKeys);
			if (iRet)
				return iRet;
		}
		// We don't want the key, but this window has the focus.
		// Therefore, force it to main reaper wnd (passthrough) so that main wnd actions work!
		return -666;
	}
	return 0;
}

// *Local* function to save the state of the view.  This is the window size, position & dock state,
// as well as any derived class view information from the function SaveView
int SWS_DockWnd::SaveState(char* cStateBuf, int iMaxLen)
{
	if (m_bLoadingState)
		return 0;

	int iLen = sizeof(SWS_DockWnd_State);
	if (IsWindow(m_hwnd))
	{
		if (!IsDocked())
			GetWindowRect(m_hwnd, &m_state.r);
		else
			m_state.whichdock = DockIsChildOfDock ? DockIsChildOfDock(m_hwnd, NULL) : 0;
	}
	if (!m_bUserClosed & IsWindow(m_hwnd))
		m_state.state |= 1;
	else
		m_state.state &= ~1;

	if (cStateBuf)
	{
		memcpy(cStateBuf, &m_state, iLen);
		iLen += SaveView(cStateBuf + iLen, iMaxLen - iLen);

		for (int i = 0; i < iLen / (int)sizeof(int); i++)
			REAPER_MAKELEINTMEM(&(((int*)cStateBuf)[i]));
	}
	else
		iLen += SaveView(NULL, 0);

	return iLen;
}

// *Local* function to restore view state.  This is the window size, position & dock state.
// Also calls the derived class method LoadView.
void SWS_DockWnd::LoadState(const char* cStateBuf, int iLen)
{
	m_bLoadingState = true;

	bool bDocked = IsDocked();
	if (cStateBuf && iLen >= sizeof(SWS_DockWnd_State))
	{
		for (int i = 0; i < sizeof(SWS_DockWnd_State) / (int)sizeof(int); i++)
			((int*)&m_state)[i] = REAPER_MAKELEINT(*((int*)cStateBuf+i));
	}
	else
		m_state.state = 0;

	if (Dock_UpdateDockID) // v4 only!
		Dock_UpdateDockID((char*)m_cId, m_state.whichdock);

	if (m_state.state & 1)
	{
		if (IsWindow(m_hwnd) && bDocked != ((m_state.state & 2) == 2) ||
			(bDocked && DockIsChildOfDock(m_hwnd, NULL) != m_state.whichdock))
			// If the window's already open, but the dock state or docker # has changed,
			// destroy and reopen.
			DestroyWindow(m_hwnd);

		Show(false, false);
	}
	else if (IsWindow(m_hwnd))
		DestroyWindow(m_hwnd);

	if (iLen > sizeof(SWS_DockWnd_State))
	{
		int iViewLen = iLen - sizeof(SWS_DockWnd_State);
		char* cTemp = new char[iViewLen];
		memcpy(cTemp, cStateBuf + sizeof(SWS_DockWnd_State), iViewLen);
		LoadView(cTemp, iViewLen);
	}
	m_bLoadingState = false;
}

void SWS_DockWnd::KillTooltip(bool doRefresh)
{
	KillTimer(m_hwnd, TOOLTIP_TIMER);
	bool had=!!m_tooltip[0];
	*m_tooltip='\0';
	if (had && doRefresh)
		InvalidateRect(m_hwnd,NULL,FALSE);
}

SWS_ListView::SWS_ListView(HWND hwndList, HWND hwndEdit, int iCols, SWS_LVColumn* pCols, const char* cINIKey, bool bTooltips)
:m_hwndList(hwndList), m_hwndEdit(hwndEdit), m_hwndTooltip(NULL), m_iSortCol(1), m_iEditingItem(-1), m_iEditingCol(0),
  m_iCols(iCols), m_pCols(NULL), m_pDefaultCols(pCols), m_bDisableUpdates(false), m_cINIKey(cINIKey),
#ifndef _WIN32
  m_pClickedItem(NULL)
#else
  m_dwSavedSelTime(0),m_bShiftSel(false)
#endif
{
	SetWindowLongPtr(hwndList, GWLP_USERDATA, (LONG_PTR)this);
	if (m_hwndEdit)
		SetWindowLongPtr(m_hwndEdit, GWLP_USERDATA, 0xdeadf00b);

	// Load sort and column data
	m_pCols = new SWS_LVColumn[m_iCols];
	memcpy(m_pCols, m_pDefaultCols, sizeof(SWS_LVColumn) * m_iCols);

	char cDefaults[256];
	sprintf(cDefaults, "%d", m_iSortCol);
	int iPos = 0;
	for (int i = 0; i < m_iCols; i++)
		_snprintf(cDefaults + strlen(cDefaults), 64-strlen(cDefaults), " %d %d", m_pCols[i].iWidth, m_pCols[i].iPos != -1 ? iPos++ : -1);
	char str[256];
	GetPrivateProfileString(SWS_INI, m_cINIKey, cDefaults, str, 256, get_ini_file());
	LineParser lp(false);
	if (!lp.parse(str))
	{
		m_iSortCol = lp.gettoken_int(0);
		iPos = 0;
		for (int i = 0; i < m_iCols; i++)
		{
			int iWidth = lp.gettoken_int(i*2+1);
			if (iWidth)
			{
				m_pCols[i].iWidth = lp.gettoken_int(i*2+1);
				m_pCols[i].iPos = lp.gettoken_int(i*2+2);
				iPos = m_pCols[i].iPos;
			}
			else if (m_pCols[i].iPos != -1) // new cols are invisible?
			{
				m_pCols[i].iPos = iPos++;
			}
		}
	}

#ifdef _WIN32
	ListView_SetExtendedListViewStyle(hwndList, LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP);

	// Setup UTF8
	WDL_UTF8_HookListView(hwndList);

	// Create the tooltip window (if it's necessary)
	if (bTooltips)
	{
		m_hwndTooltip = CreateWindowEx(WS_EX_TOPMOST, TOOLTIPS_CLASS, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,                
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_hwndList, NULL, g_hInst, NULL );
		SetWindowPos(m_hwndTooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
#else
	EnableColumnResize(hwndList);
#endif
	
	ShowColumns();

	// Set colors
	//ListView_SetBkColor(hwndList, GSC_mainwnd(COLOR_WINDOW));
	//ListView_SetTextBkColor(hwndList, GSC_mainwnd(COLOR_WINDOW));
	//ListView_SetTextColor(hwndList, GSC_mainwnd(COLOR_BTNTEXT));

	// Need to call Update(); when ready elsewhere
}

SWS_ListView::~SWS_ListView()
{
	delete [] m_pCols;
}

SWS_ListItem* SWS_ListView::GetListItem(int index, int* iState)
{
	if (index < 0)
		return NULL;
	LVITEM li;
	li.mask = LVIF_PARAM | (iState ? LVIF_STATE : 0);
	li.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
	li.iItem = index;
	li.iSubItem = 0;
	ListView_GetItem(m_hwndList, &li);
	if (iState)
		*iState = li.state;
	return (SWS_ListItem*)li.lParam;
}

bool SWS_ListView::IsSelected(int index)
{
	if (index < 0)
		return false;
	return ListView_GetItemState(m_hwndList, index, LVIS_SELECTED) ? true : false;
}

SWS_ListItem* SWS_ListView::EnumSelected(int* i)
{
	if (!m_hwndList)
		return NULL;

	int temp = 0;
	if (!i)
		i = &temp;
	LVITEM li;
	li.mask = LVIF_PARAM | LVIF_STATE;
	li.stateMask = LVIS_SELECTED;
	li.iSubItem = 0;

	while (*i < ListView_GetItemCount(m_hwndList))
	{
		li.iItem = (*i)++;
		ListView_GetItem(m_hwndList, &li);
		if (li.state)
			return (SWS_ListItem*)li.lParam;
	}
	return NULL;
}

bool SWS_ListView::SelectByItem(SWS_ListItem* _item)
{
	for (int i = 0; i < GetListItemCount(); i++)
	{
		SWS_ListItem* item = GetListItem(i);
		if (item == _item) 
		{
			ListView_SetItemState(m_hwndList, -1, 0, LVIS_SELECTED);
			ListView_SetItemState(m_hwndList, i, LVIS_SELECTED, LVIS_SELECTED); 
			ListView_EnsureVisible(m_hwndList, i, true);
			return true;
		}
	}
	return false;
}

int SWS_ListView::OnNotify(WPARAM wParam, LPARAM lParam)
{
	NMLISTVIEW* s = (NMLISTVIEW*)lParam;

#ifdef _WIN32
	if (!m_bDisableUpdates && s->hdr.code == LVN_ITEMCHANGING && s->iItem >= 0 && (s->uNewState ^ s->uOldState) & LVIS_SELECTED)
	{
		// These calls are made in big groups, save the cur state on the first call
		// so we can restore it later if necessary
		if (GetTickCount() - m_dwSavedSelTime > 20)
		{
			m_pSavedSel.Resize(ListView_GetItemCount(m_hwndList), false);
			for (int i = 0; i < m_pSavedSel.GetSize(); i++)
				m_pSavedSel.Get()[i] = ListView_GetItemState(m_hwndList, i, LVIS_SELECTED | LVIS_FOCUSED);
			m_dwSavedSelTime = GetTickCount();
			m_bShiftSel = GetAsyncKeyState(VK_SHIFT) & 0x8000 ? true : false;
		}
		
		int iRet = OnItemSelChanging(GetListItem(s->iItem), s->uNewState & LVIS_SELECTED ? true : false);
		SetWindowLongPtr(GetParent(m_hwndList), DWLP_MSGRESULT, iRet);
		return iRet;
	}
#endif
	if (!m_bDisableUpdates && s->hdr.code == LVN_ITEMCHANGED && s->iItem >= 0)
	{
		if (s->uChanged & LVIF_STATE && (s->uNewState ^ s->uOldState) & LVIS_SELECTED)
			OnItemSelChanged(GetListItem(s->iItem), s->uNewState);

#ifndef _WIN32
		// See OSX comments in NM_CLICK below

		// Send the full compliment of OnItemSelChange messges, either from the saved array or the curent state
		if (m_pSavedSel.GetSize() && m_pSavedSel.GetSize() == ListView_GetItemCount(m_hwndList))
		{	// Restore the "correct" selection
			for (int i = 0; i < m_pSavedSel.GetSize(); i++)
				ListView_SetItemState(m_hwndList, i, m_pSavedSel.Get()[i], LVIS_SELECTED | LVIS_FOCUSED);
			m_pSavedSel.Resize(0, false);
		}
		else
		{
			// Send OnItemSelChange messages for everything on the list
			for (int i = 0; i < ListView_GetItemCount(m_hwndList); i++)
			{
				int iState;
				SWS_ListItem* item = GetListItem(i, &iState);
				OnItemSelChanged(item, iState);
			}
		}
	
		if (m_pClickedItem)
		{
			OnItemBtnClk(m_pClickedItem, m_iClickedCol, m_iClickedKeys);
			m_pClickedItem = NULL;
		}
#endif
		return 0;
	}
	else if (s->hdr.code == NM_CLICK)
	{
		// Ignore clicks if editing (SWELL sends an extra NM_CLICK after NM_DBLCLK)
		if (m_iEditingItem != -1)
			return 0;
		
		int iDataCol = DisplayToDataCol(s->iSubItem);
#ifdef _WIN32
		int iKeys = ((NMITEMACTIVATE*)lParam)->uKeyFlags;
#else
		int iKeys = SWS_GetModifiers();
#endif
		// Call the std click handler
		OnItemClk(GetListItem(s->iItem), iDataCol, iKeys);

		// Then do some extra work for the item button click handler
		if (s->iItem >= 0 && m_pCols[iDataCol].iType & 2)
		{	// Clicked on an item "button"
#ifdef _WIN32
			if ((GetTickCount() - m_dwSavedSelTime < 20 || (iKeys & LVKF_SHIFT)) && m_pSavedSel.GetSize() == ListView_GetItemCount(m_hwndList) && m_pSavedSel.Get()[s->iItem] & LVIS_SELECTED)
			{
				bool bSaveDisableUpdates = m_bDisableUpdates;
				m_bDisableUpdates = true;

				// If there's a valid saved selection, and the user clicked on a selected track, the restore that selection
				for (int i = 0; i < m_pSavedSel.GetSize(); i++)
				{
					OnItemSelChanged(GetListItem(i), m_pSavedSel.Get()[i]);
					ListView_SetItemState(m_hwndList, i, m_pSavedSel.Get()[i], LVIS_SELECTED | LVIS_FOCUSED);
				}

				m_bDisableUpdates = bSaveDisableUpdates;
			}
			else if (m_bShiftSel)
			{
				// Ignore shift if the selection changed (because of a shift key hit)
				iKeys &= ~LVKF_SHIFT;
				m_bShiftSel = false;
			}
			OnItemBtnClk(GetListItem(s->iItem), iDataCol, iKeys);
#else
			// In OSX NM_CLICK comes *before* the changed notification.
			// Cases:
			// 1 - the user clicked on a non-selected item
			//     Call OnBtnClk later, in the LVN_CHANGE handler
			// 2 - one item is selected, and the user clicked on that.
			//     Call OnBtnClk now, because no change will be sent later
			// 3 - more than one item is selected, user clicked a selected one
			//     Call OnBtnClk now.  LVN_CHANGE is called later, and change the selection
			//     back to where it should be in that handler
			
			int iState;
			SWS_ListItem* item = GetListItem(s->iItem, &iState);
			m_iClickedKeys = iKeys;
			
			// Case 1:
			if (!(iState & LVIS_SELECTED))
			{
				m_pClickedItem = item;
				m_iClickedCol = iDataCol;
				m_iClickedKeys &= (LVKF_ALT | LVKF_CONTROL); // Ignore shift later
				return 0;
			}
		
			if (ListView_GetSelectedCount(m_hwndList) == 1)
			{	// Case 2:
				OnItemBtnClk(item, iDataCol, m_iClickedKeys);
				m_pSavedSel.Resize(0, false); // Saved sel size of zero means to not reset
			}
			else
			{	// Case 3:
				// Save the selections to be restored later
				m_pSavedSel.Resize(ListView_GetItemCount(m_hwndList), false);
				for (int i = 0; i < m_pSavedSel.GetSize(); i++)
					m_pSavedSel.Get()[i] = iState;
				OnItemBtnClk(item, iDataCol, m_iClickedKeys);
			}
#endif
		}
	}
	else if (s->hdr.code == NM_DBLCLK && s->iItem >= 0)
	{
		int iDataCol = DisplayToDataCol(s->iSubItem);
		if (iDataCol >= 0 && iDataCol < m_iCols && m_pCols[iDataCol].iType & 1 && 
			IsEditListItemAllowed(GetListItem(s->iItem), iDataCol))
		{
			EditListItem(s->iItem, iDataCol);
		}
		else
			OnItemDblClk(GetListItem(s->iItem), iDataCol);
	}
	else if (s->hdr.code == LVN_COLUMNCLICK)
	{
		int iDataCol = DisplayToDataCol(s->iSubItem);
		if (iDataCol+1 == abs(m_iSortCol))
			m_iSortCol = -m_iSortCol;
		else
			m_iSortCol = iDataCol + 1;
		Sort();
	}
	else if (s->hdr.code == LVN_BEGINDRAG)
	{
		EditListItemEnd(true);
		OnBeginDrag(GetListItem(s->iItem));
	}
	/*else if (s->hdr.code == NM_CUSTOMDRAW) // TODO for coloring of the listview
	{
		LPNMLVCUSTOMDRAW lplvcd = (LPNMLVCUSTOMDRAW)lParam;

		switch(lplvcd->nmcd.dwDrawStage) 
		{
			case CDDS_PREPAINT : //Before the paint cycle begins
				SetWindowLong(hwndDlg, DWL_MSGRESULT, CDRF_NOTIFYITEMDRAW);
				return 1;
			case CDDS_ITEMPREPAINT: //Before an item is drawn
				lplvcd->clrText = GSC_mainwnd(COLOR_BTNTEXT);
				lplvcd->clrTextBk = GSC_mainwnd(COLOR_WINDOW);
				lplvcd->clrFace = RGB(0,0,0);
				SetWindowLong(hwndDlg, DWL_MSGRESULT, CDRF_NEWFONT);
				return 1;
			default:
				SetWindowLong(hwndDlg, DWL_MSGRESULT, CDRF_DODEFAULT);
				return 1;
		}
	}*/
	return 0;
}

void SWS_ListView::OnDestroy()
{
	// For safety
	EditListItemEnd(false);

	// Save cols
	char str[256];
	int cols[20];
	int iCols = 0;
	for (int i = 0; i < m_iCols; i++)
		if (m_pCols[i].iPos != -1)
			iCols++;
#ifdef _WIN32
	HWND header = ListView_GetHeader(m_hwndList);
	Header_GetOrderArray(header, iCols, &cols);
#else
	for (int i = 0; i < iCols; i++)
		cols[i] = i;
#endif
	iCols = 0;

	for (int i = 0; i < m_iCols; i++)
		if (m_pCols[i].iPos != -1)
			m_pCols[i].iPos = cols[iCols++];
		
	sprintf(str, "%d", m_iSortCol);

	iCols = 0;
	for (int i = 0; i < m_iCols; i++)
		if (m_pCols[i].iPos >= 0)
			_snprintf(str + strlen(str), 256-strlen(str), " %d %d", ListView_GetColumnWidth(m_hwndList, iCols++), m_pCols[i].iPos);
		else
			_snprintf(str + strlen(str), 256-strlen(str), " %d %d", m_pCols[i].iWidth, m_pCols[i].iPos);

	WritePrivateProfileString(SWS_INI, m_cINIKey, str, get_ini_file());

	if (m_hwndTooltip)
	{
		DestroyWindow(m_hwndTooltip);
		m_hwndTooltip = NULL;
	}
}

int SWS_ListView::EditingKeyHandler(MSG *msg)
{
	if (msg->message == WM_KEYDOWN && m_iEditingItem != -1)
	{
		bool bShift = GetAsyncKeyState(VK_SHIFT)   & 0x8000 ? true : false;

		if (msg->wParam == VK_ESCAPE)
		{
			EditListItemEnd(false);
			return 1;
		}
		else if (msg->wParam == VK_TAB)
		{
			int iItem = m_iEditingItem;
			DisableUpdates(true);
			EditListItemEnd(true, false);
			if (!bShift)
			{
				if (++iItem >= ListView_GetItemCount(m_hwndList))
					iItem = 0;
			}
			else
			{
				if (--iItem < 0)
					iItem = ListView_GetItemCount(m_hwndList) - 1;
			}
			EditListItem(GetListItem(iItem), m_iEditingCol);
			DisableUpdates(false);
			return 1;
		}
		else if (msg->wParam == VK_RETURN)
		{
			EditListItemEnd(true);
			return 1;
		}
		
		// All other keystrokes when editing should go to the control, not to SWS_DockWnd or Reaper
		// Fixes bug where the delete key when editing would delete the line, but it would still exist temporarily
		return -1;
	}
	return 0;
}

int SWS_ListView::LVKeyHandler(MSG* msg, int iKeyState)
{
	// Catch a few keys that are handled natively by the list view
	if (msg->message == WM_KEYDOWN)
	{
		if (msg->wParam == VK_UP || msg->wParam == VK_DOWN || msg->wParam == VK_TAB)
			return -1;
#ifdef _WIN32
		else if (msg->wParam == 'A' && iKeyState == LVKF_CONTROL && !(GetWindowLongPtr(m_hwndList, GWL_STYLE) & LVS_SINGLESEL))
		{
			for (int i = 0; i < ListView_GetItemCount(m_hwndList); i++)
				ListView_SetItemState(m_hwndList, i, LVIS_SELECTED, LVIS_SELECTED);
			return 1;
		}
#endif
	}
	return 0;
}

void SWS_ListView::Update()
{
	// Fill in the data by pulling it from the derived class
	if (m_iEditingItem == -1 && !m_bDisableUpdates)
	{
		m_bDisableUpdates = true;
		char str[256];

		bool bResort = false;
		static int iLastSortCol = -999;
		if (m_iSortCol != iLastSortCol)
		{
			iLastSortCol = m_iSortCol;
			bResort = true;
		}

		SWS_ListItemList items;
		GetItemList(&items);

		if (!items.GetSize())
			ListView_DeleteAllItems(m_hwndList);

		// The list is sorted, use that to our advantage here:
		int lvItemCount = ListView_GetItemCount(m_hwndList);
		int newIndex = lvItemCount;
		for (int i = 0; items.GetSize() || i < lvItemCount; i++)
		{
			bool bFound = false;
			SWS_ListItem* pItem;
			if (i < lvItemCount)
			{	// First check items in the listview, match to item list
				pItem = GetListItem(i);
				int iIndex = items.Find(pItem);
				if (iIndex == -1)
				{
					// Delete items from listview that aren't in the item list
					ListView_DeleteItem(m_hwndList, i);
					i--;
					lvItemCount--;
					newIndex--;
					continue;
				}
				else
				{
					// Delete item from item list to indicate "used"
					items.Delete(iIndex);
					bFound = true;
				}
			}
			else
			{	// Items left in the item list are new
				pItem = items.Remove();
			}

			// We have an item pointer, and a listview index, add/edit the listview
			// Update the list, no matter what, because text may have changed
			LVITEM item;
			item.mask = 0;
			int iNewState = GetItemState(pItem);
			if (iNewState >= 0)
			{
				int iCurState = bFound ? ListView_GetItemState(m_hwndList, i, LVIS_SELECTED | LVIS_FOCUSED) : 0;
				if (iNewState && !(iCurState & LVIS_SELECTED))
				{
					item.mask |= LVIF_STATE;
					item.state = LVIS_SELECTED;
					item.stateMask = LVIS_SELECTED;
				}
				else if (!iNewState && (iCurState & LVIS_SELECTED))
				{
					item.mask |= LVIF_STATE;
					item.state = 0;
					item.stateMask = LVIS_SELECTED | ((iCurState & LVIS_FOCUSED) ? LVIS_FOCUSED : 0);
				}
			}

			item.iItem = bFound ? i : newIndex++;
			item.pszText = str;

			int iCol = 0;
			for (int k = 0; k < m_iCols; k++)
				if (m_pCols[k].iPos != -1)
				{
					item.iSubItem = iCol;
					GetItemText(pItem, k, str, 256);
					if (!iCol && !bFound)
					{
						item.mask |= LVIF_PARAM | LVIF_TEXT;
						item.lParam = (LPARAM)pItem;
						ListView_InsertItem(m_hwndList, &item);
						bResort = true;
					}
					else
					{
						char curStr[256];
						ListView_GetItemText(m_hwndList, item.iItem, iCol, curStr, 256);
						if (strcmp(str, curStr))
							item.mask |= LVIF_TEXT;
						if (item.mask)
						{
							// Only set if there's changes
							// May be less efficient here, but less messages get sent for sure!
							ListView_SetItem(m_hwndList, &item);
							bResort = true;
						}
					}
					item.mask = 0;
					iCol++;
				}
		}

		if (bResort)
			Sort();

#ifdef _WIN32
		if (m_hwndTooltip)
		{
			TOOLINFO ti = { sizeof(TOOLINFO), };
			ti.lpszText = str;
			ti.hwnd = m_hwndList;
			ti.uFlags = TTF_SUBCLASS;
			ti.hinst  = g_hInst;

			// Delete all existing tools
			while (SendMessage(m_hwndTooltip, TTM_ENUMTOOLS, 0, (LPARAM)&ti))
				SendMessage(m_hwndTooltip, TTM_DELTOOL, 0, (LPARAM)&ti);

			RECT r;
			// Add tooltips after sort
			for (int i = 0; i < ListView_GetItemCount(m_hwndList); i++)
			{
				// Get the rect of the line
				ListView_GetItemRect(m_hwndList, i, &r, LVIR_BOUNDS);
				memcpy(&ti.rect, &r, sizeof(RECT));
				ti.uId = i;
				GetItemTooltip(GetListItem(i), str, 100);
				SendMessage(m_hwndTooltip, TTM_ADDTOOL, 0, (LPARAM)&ti);
			}
		}
#endif
		m_bDisableUpdates = false;
	}
}

// Return TRUE if a the column header was clicked
bool SWS_ListView::DoColumnMenu(int x, int y)
{
	int iCol;
	SWS_ListItem* item = GetHitItem(x, y, &iCol);
	if (!item && iCol != -1)
	{
		EditListItemEnd(true); // fix possible crash

		HMENU hMenu = CreatePopupMenu();
		AddToMenu(hMenu, "Visible columns", 0);
		EnableMenuItem(hMenu, 0, MF_BYPOSITION | MF_GRAYED);

		for (int i = 0; i < m_iCols; i++)
		{
			AddToMenu(hMenu, m_pCols[i].cLabel, i + 1);
			if (m_pCols[i].iPos != -1)
				CheckMenuItem(hMenu, i+1, MF_BYPOSITION | MF_CHECKED);
		}
		AddToMenu(hMenu, SWS_SEPARATOR, 0);
		AddToMenu(hMenu, "Reset", m_iCols + 1);

		int iCol = TrackPopupMenu(hMenu, TPM_RETURNCMD, x, y, 0, m_hwndList, NULL);
		DestroyMenu(hMenu);

		if (iCol)
		{
			iCol--;
			if (iCol == m_iCols)
			{
				memcpy(m_pCols, m_pDefaultCols, sizeof(SWS_LVColumn) * m_iCols);
				int iPos = 0;
				for (int i = 0; i < m_iCols; i++)
					if (m_pCols[i].iPos != -1)
						m_pCols[i].iPos = iPos++;
			}
			else
			{
				// Save all visible column widths
				for (int i = 0; i < m_iCols; i++)
					if (m_pCols[i].iPos != -1)
						m_pCols[i].iWidth = ListView_GetColumnWidth(m_hwndList, DataToDisplayCol(i));

				if (m_pCols[iCol].iPos == -1)
				{	// Adding column
					for (int i = 0; i < m_iCols; i++)
						if (m_pCols[i].iPos >= iCol)
							m_pCols[i].iPos++;
					m_pCols[iCol].iPos = iCol;
				}
				else
				{	// Deleting column
					int iDelPos = m_pCols[iCol].iPos;
					m_pCols[iCol].iPos = -1;
					for (int i = 0; i < m_iCols; i++)
						if (m_pCols[i].iPos > iDelPos)
							m_pCols[i].iPos--;

					// Fix the sort column
					if (abs(m_iSortCol) - 1 == iCol)
						for (int i = 0; i < m_iCols; i++)
							if (m_pCols[i].iPos != -1)
							{
								m_iSortCol = i+1;
								break;
								// Possible to break out leaving the sort column pointing to
								// an invisible col if there's no columns shown!
							}
				}
			}

			ListView_DeleteAllItems(m_hwndList);
			while(ListView_DeleteColumn(m_hwndList, 0));
			ShowColumns();
			Update();
		}
		return true;
	}
	return false;
}

SWS_ListItem* SWS_ListView::GetHitItem(int x, int y, int* iCol)
{
	LVHITTESTINFO ht;
	POINT pt = { x, y };
	ht.pt = pt;
	ht.flags = LVHT_ONITEM;
	ScreenToClient(m_hwndList, &ht.pt);
	int iItem = ListView_SubItemHitTest(m_hwndList, &ht);

#ifdef _WIN32
	RECT r;
	HWND header = ListView_GetHeader(m_hwndList);
	GetWindowRect(header, &r);
	if (PtInRect(&r, pt))
#else
	if (ht.pt.y < 0)
#endif
	{
		if (iCol)
			*iCol = ht.iSubItem != -1 ? DisplayToDataCol(ht.iSubItem) : 0; // iCol != -1 means "header", set 0 for "unknown column"
		return NULL;
	}
	else if (iItem >= 0 
#ifdef _WIN32 //JFB added: other "no mans land" but ListView_IsItemVisible() is not part of SWELL!
		&& ListView_IsItemVisible(m_hwndList, iItem)
#endif
		)
	{
		if (iCol)
			*iCol = DisplayToDataCol(ht.iSubItem);
		return GetListItem(iItem);
	}
	if (iCol)
		*iCol = -1; // -1 + NULL ret means "no mans land"
	return NULL;
}

void SWS_ListView::EditListItem(SWS_ListItem* item, int iCol)
{
	// Convert to index and call edit
#ifdef _WIN32
	LVFINDINFO fi;
	fi.flags = LVFI_PARAM;
	fi.lParam = (LPARAM)item;
	int iItem = ListView_FindItem(m_hwndList, -1, &fi);
#else
	int iItem = -1;
	LVITEM li;
	li.mask = LVIF_PARAM;
	for (int i = 0; i < ListView_GetItemCount(m_hwndList); i++)
	{
		li.iItem = i;
		ListView_GetItem(m_hwndList, &li);
		if ((SWS_ListItem*)li.lParam == item)
		{
			iItem = i;
			break;
		}
	}
#endif
	if (iItem >= 0)
		EditListItem(iItem, iCol);
}

void SWS_ListView::EditListItem(int iIndex, int iCol)
{
	RECT r;
	m_iEditingItem = iIndex;
	m_iEditingCol = iCol;
	int iDispCol = DataToDisplayCol(iCol);
	ListView_GetSubItemRect(m_hwndList, iIndex, iDispCol, LVIR_LABEL, &r);

	RECT sr = r;
	ClientToScreen(m_hwndList, (LPPOINT)&sr);
	ClientToScreen(m_hwndList, ((LPPOINT)&sr)+1);

	// clamp to list view width
	GetWindowRect(m_hwndList, &r);
#ifdef _WIN32
	sr.left = max(r.left+(GetSystemMetrics(SM_CXEDGE)*2), sr.left);
	sr.right = min(r.right-(GetSystemMetrics(SM_CXEDGE)*2), sr.right);
	sr.top += 1; // do not hide top grid line
#else
/* JFB!!! commented: needed on OSX too? cannot test..
	sr.left = max(r.left+2, sr.left);
	sr.right = min(r.right-2, sr.right);
	sr.top += 1; // do not hide top grid line
*/
#endif

	HWND hDlg = GetParent(m_hwndEdit);
	ScreenToClient(hDlg, (LPPOINT)&sr);
	ScreenToClient(hDlg, ((LPPOINT)&sr)+1);

	// Create a new edit control to go over that rect
	int lOffset = -1;
	SetWindowPos(m_hwndEdit, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	SetWindowPos(m_hwndEdit, HWND_TOP, sr.left+lOffset, sr.top, sr.right-sr.left, sr.bottom-sr.top, SWP_SHOWWINDOW | SWP_NOZORDER);
	ShowWindow(m_hwndEdit, SW_SHOW);

	SWS_ListItem* item = GetListItem(iIndex);
	char str[100];
	GetItemText(item, iCol, str, 100);
	SetWindowText(m_hwndEdit, str);
	SetFocus(m_hwndEdit);
	SendMessage(m_hwndEdit, EM_SETSEL, 0, -1);
	SetTimer(GetParent(m_hwndList), CELL_EDIT_TIMER, CELL_EDIT_TIMEOUT, NULL);
}

bool SWS_ListView::EditListItemEnd(bool bSave, bool bResort)
{
	bool updated = false;
	if (m_iEditingItem != -1 && IsWindow(m_hwndList) && IsWindow(m_hwndEdit))
	{
		KillTimer(GetParent(m_hwndList), CELL_EDIT_TIMER);
		if (bSave)
		{
			char newStr[100];
			char curStr[100];
			GetWindowText(m_hwndEdit, newStr, 100);
			SWS_ListItem* item = GetListItem(m_iEditingItem);
			GetItemText(item, m_iEditingCol, curStr, 100);
			if (strcmp(curStr, newStr))
			{
				SetItemText(item, m_iEditingCol, newStr);
				GetItemText(item, m_iEditingCol, newStr, 100);
				ListView_SetItemText(m_hwndList, m_iEditingItem, DataToDisplayCol(m_iEditingCol), newStr);
				updated = true;
			}
			if (bResort)
				ListView_SortItems(m_hwndList, sListCompare, (LPARAM)this);
			// TODO resort? Just call update?
			// Update is likely called when SetItemText is called too...
		}
		m_iEditingItem = -1;
		ShowWindow(m_hwndEdit, SW_HIDE);
		SetFocus(m_hwndList);
	}
	return updated;
}

int SWS_ListView::OnEditingTimer()
{
	if (m_iEditingItem == -1 || GetFocus() != m_hwndEdit)
		EditListItemEnd(true);

	return 0;
}

int SWS_ListView::OnItemSort(SWS_ListItem* item1, SWS_ListItem* item2)
{
	// Just sort by string
	char str1[64];
	char str2[64];
	GetItemText(item1, abs(m_iSortCol)-1, str1, 64);
	GetItemText(item2, abs(m_iSortCol)-1, str2, 64);

	// If strings are purely numbers, sort numerically
	char* pEnd1, *pEnd2;
	int i1 = strtol(str1, &pEnd1, 0);
	int i2 = strtol(str2, &pEnd2, 0);
	int iRet = 0;
	if ((i1 || i2) && !*pEnd1 && !*pEnd2)
	{
		if (i1 > i2)
			iRet = 1;
		else if (i1 < i2)
			iRet = -1;
	}
	else
		iRet = _stricmp(str1, str2);
	
	if (m_iSortCol < 0)
		return -iRet;
	else
		return iRet;
}

void SWS_ListView::ShowColumns()
{
	LVCOLUMN col;
	col.mask = LVCF_TEXT | LVCF_WIDTH;

	int iCol = 0;
	for (int i = 0; i < m_iCols; i++)
	{
		if (m_pCols[i].iPos >= 0)
		{
			col.pszText = (char*)m_pCols[i].cLabel;
			col.cx = m_pCols[i].iWidth;
			ListView_InsertColumn(m_hwndList, iCol, &col);
			iCol++;
		}
	}

	int cols[20];
	int iCols = 0;
	for (int i = 0; i < m_iCols; i++)
		if (m_pCols[i].iPos != -1)
			cols[iCols++] = m_pCols[i].iPos;
#ifdef _WIN32
	HWND header = ListView_GetHeader(m_hwndList);
	Header_SetOrderArray(header, iCols, &cols);
#endif
}

void SWS_ListView::Sort()
{
	ListView_SortItems(m_hwndList, sListCompare, (LPARAM)this);
	int iCol = abs(m_iSortCol) - 1;
	iCol = DataToDisplayCol(iCol) + 1;
	if (m_iSortCol < 0)
		iCol = -iCol;
	SetListviewColumnArrows(iCol);
}

void SWS_ListView::SetListviewColumnArrows(int iSortCol)
{
#ifdef _WIN32
	// Set the column arrows
	HWND header = ListView_GetHeader(m_hwndList);
	for (int i = 0; i < Header_GetItemCount(header); i++)
	{
		HDITEM hi;
		hi.mask = HDI_FORMAT | HDI_BITMAP;
		Header_GetItem(header, i, &hi);
		if (hi.hbm)
			DeleteObject(hi.hbm);
		hi.fmt &= ~(HDI_BITMAP|HDF_BITMAP_ON_RIGHT|HDF_SORTDOWN|HDF_SORTUP);

		if (IsCommCtrlVersion6())
		{
			if (iSortCol == i+1)
                hi.fmt |= HDF_SORTUP;
			else if (-iSortCol == i+1)
                hi.fmt |= HDF_SORTDOWN;
		}
		else
		{
			if (iSortCol == i+1)
			{
				hi.fmt |= HDF_BITMAP|HDF_BITMAP_ON_RIGHT;
				hi.hbm = (HBITMAP)LoadImage(g_hInst, MAKEINTRESOURCE(IDB_UP), IMAGE_BITMAP, 0,0, LR_LOADMAP3DCOLORS);
			}
			else if (-iSortCol == i+1)
			{
				hi.fmt |= HDF_BITMAP|HDF_BITMAP_ON_RIGHT;
				hi.hbm = (HBITMAP)LoadImage(g_hInst, MAKEINTRESOURCE(IDB_DOWN), IMAGE_BITMAP, 0,0, LR_LOADMAP3DCOLORS);
			}
		}
		Header_SetItem(header, i, &hi);
	}
#else
	SetColumnArrows(m_hwndList, iSortCol);
#endif
}

int SWS_ListView::DisplayToDataCol(int iCol)
{	// The display column # can be potentially less than the data column if there are hidden columns
	if (iCol < 0)
		return iCol;
	int iVis = 0;
	int iDataCol = iCol;
	for (int i = 0; i < m_iCols && iVis <= iCol; i++)
		if (m_pCols[i].iPos == -1)
			iDataCol++;
		else
			iVis++;
	return iDataCol;
}

int SWS_ListView::DataToDisplayCol(int iCol)
{	// The data col # can be more than the display col if there are hidden columns
	for (int i = 0; i < iCol; i++)
		if (m_pCols[i].iPos == -1)
			iCol--;
	return iCol;
}

int SWS_ListView::sListCompare(LPARAM lParam1, LPARAM lParam2, LPARAM lSortParam)
{
	SWS_ListView* pLV = (SWS_ListView*)lSortParam;
	return pLV->OnItemSort((SWS_ListItem*)lParam1, (SWS_ListItem*)lParam2);
}


///////////////////////////////////////////////////////////////////////////////
// Code bits courtesy of Cockos. Thank you Cockos!
///////////////////////////////////////////////////////////////////////////////

// From Justin: http://askjf.com/index.php?q=1609s
// JFB mod: clamp grid lines to nb of displayed rows + GUI glitch fix for SWS_ListView 
//          before mod: http://stash.reaper.fm/11297/fixed_glitch.gif
//          after mod:  http://stash.reaper.fm/11298/fixed_glitch2.gif
#ifdef _WIN32
void DrawListCustomGridLines(HWND hwnd, HDC hdc, RECT br, int color, int ncol)
{
// JFB added --->
  int cnt = ListView_GetItemCount(hwnd);
  if (!cnt) return;
// <---

  int i;
  HPEN pen = CreatePen(PS_SOLID,0,color);
  HGDIOBJ oldObj = SelectObject(hdc,pen);
  for(i=0;i<ncol;i++)
  {
    RECT r;
    if (!ListView_GetSubItemRect(hwnd,0,i,LVIR_BOUNDS,&r) && i) break;
    r.left--;
    r.right--;
    if (!i)
    {
      int h =r.bottom-r.top;
/* JFB commented: cannot be 0 here, no grid lines when the list is empty --->
      if (!ListView_GetItemCount(hwnd)) 
      {
        r.top = 0;
        HWND head=ListView_GetHeader(hwnd);
        if (head)
        {
          GetWindowRect(head,&r);
          r.top=r.bottom-r.top;
        }
        h=17;// todo getsystemmetrics
      }
<--- */
      if (h>0)
      {
//JFB mod --->
        int row=0;
        while (r.top < br.bottom && row++ <= cnt)
//        while (r.top < br.bottom)
// <---
        {
          if (r.top >= br.top)
          {
            MoveToEx(hdc,br.left,r.top,NULL);
            LineTo(hdc,br.right,r.top);
          }
          r.top +=h;
        }
      }
    }
    else if (r.right >= br.left && r.left < br.right)
    {
/*JFB commented: i==NULL is impossible here
      if (i)
*/
      {
/*JFB commented: fix for missing grid lines (i.e. paint both right & left vertical lines in case some columns have been moved/hidden)
//               could be improved using SWS_ListView..
        if (i==1)
*/
        {
          MoveToEx(hdc,r.left,br.top,NULL);
//JFB mod --->
          LineTo(hdc,r.left,min(br.bottom, r.top+(r.bottom-r.top)*cnt));
//          LineTo(hdc,r.left,br.bottom);
// <---
        }
		MoveToEx(hdc,r.right,br.top,NULL);
//JFB mod --->
        LineTo(hdc,r.right,min(br.bottom, r.top+(r.bottom-r.top)*cnt));
//        LineTo(hdc,r.right,br.bottom);
// <---
      }
    }
  }
  SelectObject(hdc,oldObj);
  DeleteObject(pen);
}
#endif

bool ListView_HookThemeColorsMessage(HWND hwndDlg, int uMsg, LPARAM lParam, int cstate[LISTVIEW_COLORHOOK_STATESIZE], int listID, int whichTheme, int wantGridForColumns)
{
//JFB added --->
#ifndef _WIN32
  wantGridForColumns=0; //JFB!!! no grid lines on OSX yet (cannot test!)
#endif
  int sz;
  ColorTheme* ctheme = (ColorTheme*)GetColorThemeStruct(&sz);
  if (!ctheme || sz < sizeof(ColorTheme))
	  return false;
// <---

  // if whichTheme&1, is tree view
  switch (uMsg)
  {
    case WM_PAINT:
      {
        int c1=RGB(255,255,255);
        int c2=RGB(0,0,0);
        int c3=RGB(224,224,224);

#ifndef _WIN32
        int selcols[4];
        selcols[0]=ctheme->genlist_sel[0];
        selcols[1]=ctheme->genlist_sel[1];
        selcols[2]=ctheme->genlist_selinactive[0];
        selcols[3]=ctheme->genlist_selinactive[1];
#endif
        if ((whichTheme&~1) == 0)
        {
          c1 = ctheme->genlist_bg;
          c2 = ctheme->genlist_fg;
          c3 = ctheme->genlist_gridlines;
        }
        if (cstate[0] != c1 || cstate[1] != c2 || cstate[2] != c3
#ifndef _WIN32
            || memcmp(selcols,cstate+3,4*sizeof(int))
#endif
            )
        {
          cstate[0]=c1;
          cstate[1]=c2;
          cstate[2]=c3;
          HWND h = GetDlgItem(hwndDlg,listID);
#ifndef _WIN32
          memcpy(cstate+3,selcols,4*sizeof(int));
          if (h) ListView_SetSelColors(h,selcols,4);
#endif
          if (h)
          {
            if (whichTheme&1)
            {
              TreeView_SetBkColor(h,c1);
              TreeView_SetTextColor(h,c2);
            }
            else
            {
              ListView_SetBkColor(h,c1);
              ListView_SetTextBkColor(h,c1);
              ListView_SetTextColor(h,c2);
#ifndef _WIN32
              ListView_SetGridColor(h,c3);
#endif
            }
          }
        }
      }
    break;
    case WM_CREATE:
    case WM_INITDIALOG:
      memset(cstate,0,sizeof(cstate));
    break;
#ifdef _WIN32
    case WM_NOTIFY:
      if (lParam)
      {
        NMHDR *hdr = (NMHDR *)lParam;
        bool wantThemedSelState=true;
        if (hdr->idFrom == listID) switch (hdr->code)
        {
          case NM_CUSTOMDRAW:
            if (whichTheme&1)
            {
              LPNMTVCUSTOMDRAW lptvcd = (LPNMTVCUSTOMDRAW)lParam;
              if (wantThemedSelState) switch(lptvcd->nmcd.dwDrawStage) 
              {
                case CDDS_PREPAINT:
                  SetWindowLongPtr(hwndDlg,DWLP_MSGRESULT,CDRF_NOTIFYITEMDRAW);
                return true;      
                case CDDS_ITEMPREPAINT:
                  if (wantThemedSelState&&lptvcd->nmcd.dwItemSpec)
                  {              
                    TVITEM tvi={TVIF_HANDLE|TVIF_STATE ,(HTREEITEM)lptvcd->nmcd.dwItemSpec};
                    TreeView_GetItem(hdr->hwndFrom,&tvi);
                    if(tvi.state&(TVIS_SELECTED|TVIS_DROPHILITED))
                    {
                      int bg1=ctheme->genlist_sel[0];
                      int bg2=ctheme->genlist_selinactive[0];
                      int fg1=ctheme->genlist_sel[1];
                      int fg2=ctheme->genlist_selinactive[1];

                      bool active = (tvi.state&TVIS_DROPHILITED) || GetFocus()==hdr->hwndFrom;
                      lptvcd->clrText = active ? fg1 : fg2;
                      lptvcd->clrTextBk = active ? bg1 : bg2;
                      lptvcd->nmcd.uItemState &= ~CDIS_SELECTED;
                    }
                    SetWindowLongPtr(hwndDlg,DWLP_MSGRESULT,0);
                    return true;
                  }
                break;
              }
            }
            else if (wantGridForColumns||wantThemedSelState)
            {
              LPNMLVCUSTOMDRAW lplvcd = (LPNMLVCUSTOMDRAW)lParam;
              switch(lplvcd->nmcd.dwDrawStage) 
              {
                case CDDS_PREPAINT:
                  SetWindowLongPtr(hwndDlg,DWLP_MSGRESULT,(wantGridForColumns?CDRF_NOTIFYPOSTPAINT:0)|
                                                          (wantThemedSelState?CDRF_NOTIFYITEMDRAW:0));
                return true;      
                case CDDS_ITEMPREPAINT:
                  if (wantThemedSelState)
                  {
                    int s = ListView_GetItemState(hdr->hwndFrom, (int)lplvcd->nmcd.dwItemSpec, LVIS_SELECTED|LVIS_FOCUSED);
                    if(s&LVIS_SELECTED)
                    {
                      int bg1=ctheme->genlist_sel[0];
                      int bg2=ctheme->genlist_selinactive[0];
                      int fg1=ctheme->genlist_sel[1];
                      int fg2=ctheme->genlist_selinactive[1];

                      bool active = GetFocus()==hdr->hwndFrom;
                      lplvcd->clrText = active ? fg1 : fg2;
                      lplvcd->clrTextBk = active ? bg1 : bg2;
                      lplvcd->nmcd.uItemState &= ~CDIS_SELECTED;
                    }
                    if (s&LVIS_FOCUSED)
                    {
/*JFB commented
                      // todo: theme option for colors for focus state as well?
                      if (0 && GetFocus()==hdr->hwndFrom)
                      {
                        lplvcd->clrText = BrightenColorSlightly(lplvcd->clrText);
                        lplvcd->clrTextBk = BrightenColorSlightly(lplvcd->clrTextBk);
                      }
*/
                      lplvcd->nmcd.uItemState &= ~CDIS_FOCUS;
                    }
                    SetWindowLongPtr(hwndDlg,DWLP_MSGRESULT,0);
                    return true;
                  }
                break;
                case CDDS_POSTPAINT:                  
                  if (wantGridForColumns)
                  {
                    int c1 = ctheme->genlist_gridlines;
                    int c2 = ctheme->genlist_bg;
                    if (c1 != c2)
                    {
                      DrawListCustomGridLines(hdr->hwndFrom,lplvcd->nmcd.hdc,lplvcd->nmcd.rc,c1,wantGridForColumns);
                    }
                  }
                  SetWindowLongPtr(hwndDlg,DWLP_MSGRESULT,0);
                return true;          
              }
            }
          break;
        }
      }
#endif
  }
  return false;
}

// From Justin: "this should likely go into WDL"
void DrawTooltipForPoint(LICE_IBitmap *bm, POINT mousePt, RECT *wndr, const char *text)
{
  if (!bm || !text || !text[0]) return;

    static LICE_CachedFont tmpfont;
    if (!tmpfont.GetHFont())
    {
      bool doOutLine = true;
      LOGFONT lf = {
          14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
          OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,SWSDLG_TYPEFACE
      };
      tmpfont.SetFromHFont(CreateFontIndirect(&lf),LICE_FONT_FLAG_OWNS_HFONT|LICE_FONT_FLAG_FORCE_NATIVE);                 
    }
    tmpfont.SetBkMode(TRANSPARENT);
    LICE_pixel col1 = LICE_RGBA(0,0,0,255);
//JFB mod: same tooltip color than REAPER --->
//    LICE_pixel col2 = LICE_RGBA(255,255,192,255);
    LICE_pixel col2 = LICE_RGBA(255,255,225,255);
// <---

    tmpfont.SetTextColor(col1);
    RECT r={0,};
    tmpfont.DrawText(bm,text,-1,&r,DT_CALCRECT);

    int xo = min(max(mousePt.x,wndr->left),wndr->right);
    int yo = min(max(mousePt.y + 24,wndr->top),wndr->bottom);

    if (yo + r.bottom > wndr->bottom-4) // too close to bottom, move up if possible
    {
      if (mousePt.y - r.bottom - 12 >= wndr->top)
        yo = mousePt.y - r.bottom - 12;
      else
        yo = wndr->bottom - 4 - r.bottom;

//JFB added: (try to) prevent hidden tooltip behind the mouse pointer --->
      xo += 15;
// <---
    }

    if (xo + r.right > wndr->right - 4)
      xo = wndr->right - 4 - r.right;

    r.left += xo;
    r.top += yo;
    r.right += xo;
    r.bottom += yo;
    
    int border = 3;
    LICE_FillRect(bm,r.left-border,r.top-border,r.right-r.left+border*2,r.bottom-r.top+border*2,col2,1.0f,LICE_BLIT_MODE_COPY);
    LICE_DrawRect(bm,r.left-border,r.top-border,r.right-r.left+border*2,r.bottom-r.top+border*2,col1,1.0f,LICE_BLIT_MODE_COPY);
    
    tmpfont.DrawText(bm,text,-1,&r,0);
}
