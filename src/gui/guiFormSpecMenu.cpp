/*
Minetest
Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/


#include <cstdlib>
#include <algorithm>
#include <iterator>
#include <sstream>
#include <limits>
#include "guiButton.h"
#include "guiFormSpecMenu.h"
#include "guiTable.h"
#include "constants.h"
#include "gamedef.h"
#include "client/keycode.h"
#include "util/strfnd.h"
#include <IGUICheckBox.h>
#include <IGUIEditBox.h>
#include <IGUIButton.h>
#include <IGUIStaticText.h>
#include <IGUIFont.h>
#include <IGUITabControl.h>
#include <IGUIComboBox.h>
#include "client/renderingengine.h"
#include "log.h"
#include "client/tile.h" // ITextureSource
#include "client/hud.h" // drawItemStack
#include "filesys.h"
#include "gettime.h"
#include "gettext.h"
#include "scripting_server.h"
#include "mainmenumanager.h"
#include "porting.h"
#include "settings.h"
#include "client/client.h"
#include "client/fontengine.h"
#include "util/hex.h"
#include "util/numeric.h"
#include "util/string.h" // for parseColorString()
#include "irrlicht_changes/static_text.h"
#include "client/guiscalingfilter.h"
#include "guiEditBoxWithScrollbar.h"
#include "intlGUIEditBox.h"
#include "guiHyperText.h"

#define MY_CHECKPOS(a,b)													\
	if (v_pos.size() != 2) {												\
		errorstream<< "Invalid pos for element " << a << "specified: \""	\
			<< parts[b] << "\"" << std::endl;								\
			return;															\
	}

#define MY_CHECKGEOM(a,b)													\
	if (v_geom.size() != 2) {												\
		errorstream<< "Invalid geometry for element " << a <<				\
			"specified: \"" << parts[b] << "\"" << std::endl;				\
			return;															\
	}
/*
	GUIFormSpecMenu
*/
static unsigned int font_line_height(gui::IGUIFont *font)
{
	return font->getDimension(L"Ay").Height + font->getKerningHeight();
}

inline u32 clamp_u8(s32 value)
{
	return (u32) MYMIN(MYMAX(value, 0), 255);
}

GUIFormSpecMenu::GUIFormSpecMenu(JoystickController *joystick,
		gui::IGUIElement *parent, s32 id, IMenuManager *menumgr,
		Client *client, ISimpleTextureSource *tsrc, IFormSource *fsrc, TextDest *tdst,
		const std::string &formspecPrepend,
		bool remap_dbl_click):
	GUIModalMenu(RenderingEngine::get_gui_env(), parent, id, menumgr),
	m_invmgr(client),
	m_tsrc(tsrc),
	m_client(client),
	m_formspec_prepend(formspecPrepend),
	m_form_src(fsrc),
	m_text_dst(tdst),
	m_joystick(joystick),
	m_remap_dbl_click(remap_dbl_click)
{
	current_keys_pending.key_down = false;
	current_keys_pending.key_up = false;
	current_keys_pending.key_enter = false;
	current_keys_pending.key_escape = false;

	m_doubleclickdetect[0].time = 0;
	m_doubleclickdetect[1].time = 0;

	m_doubleclickdetect[0].pos = v2s32(0, 0);
	m_doubleclickdetect[1].pos = v2s32(0, 0);

	m_tooltip_show_delay = (u32)g_settings->getS32("tooltip_show_delay");
	m_tooltip_append_itemname = g_settings->getBool("tooltip_append_itemname");
}

GUIFormSpecMenu::~GUIFormSpecMenu()
{
	removeChildren();

	for (auto &table_it : m_tables) {
		table_it.second->drop();
	}

	delete m_selected_item;
	delete m_form_src;
	delete m_text_dst;
}

void GUIFormSpecMenu::create(GUIFormSpecMenu *&cur_formspec, Client *client,
	JoystickController *joystick, IFormSource *fs_src, TextDest *txt_dest,
	const std::string &formspecPrepend)
{
	if (cur_formspec == nullptr) {
		cur_formspec = new GUIFormSpecMenu(joystick, guiroot, -1, &g_menumgr,
			client, client->getTextureSource(), fs_src, txt_dest, formspecPrepend);
		cur_formspec->doPause = false;

		/*
			Caution: do not call (*cur_formspec)->drop() here --
			the reference might outlive the menu, so we will
			periodically check if *cur_formspec is the only
			remaining reference (i.e. the menu was removed)
			and delete it in that case.
		*/

	} else {
		cur_formspec->setFormspecPrepend(formspecPrepend);
		cur_formspec->setFormSource(fs_src);
		cur_formspec->setTextDest(txt_dest);
	}
}

void GUIFormSpecMenu::removeChildren()
{
	const core::list<gui::IGUIElement*> &children = getChildren();

	while (!children.empty()) {
		(*children.getLast())->remove();
	}

	if (m_tooltip_element) {
		m_tooltip_element->remove();
		m_tooltip_element->drop();
		m_tooltip_element = nullptr;
	}
}

void GUIFormSpecMenu::setInitialFocus()
{
	// Set initial focus according to following order of precedence:
	// 1. first empty editbox
	// 2. first editbox
	// 3. first table
	// 4. last button
	// 5. first focusable (not statictext, not tabheader)
	// 6. first child element

	core::list<gui::IGUIElement*> children = getChildren();

	// in case "children" contains any NULL elements, remove them
	for (core::list<gui::IGUIElement*>::Iterator it = children.begin();
			it != children.end();) {
		if (*it)
			++it;
		else
			it = children.erase(it);
	}

	// 1. first empty editbox
	for (gui::IGUIElement *it : children) {
		if (it->getType() == gui::EGUIET_EDIT_BOX
				&& it->getText()[0] == 0) {
			Environment->setFocus(it);
			return;
		}
	}

	// 2. first editbox
	for (gui::IGUIElement *it : children) {
		if (it->getType() == gui::EGUIET_EDIT_BOX) {
			Environment->setFocus(it);
			return;
		}
	}

	// 3. first table
	for (gui::IGUIElement *it : children) {
		if (it->getTypeName() == std::string("GUITable")) {
			Environment->setFocus(it);
			return;
		}
	}

	// 4. last button
	for (core::list<gui::IGUIElement*>::Iterator it = children.getLast();
			it != children.end(); --it) {
		if ((*it)->getType() == gui::EGUIET_BUTTON) {
			Environment->setFocus(*it);
			return;
		}
	}

	// 5. first focusable (not statictext, not tabheader)
	for (gui::IGUIElement *it : children) {
		if (it->getType() != gui::EGUIET_STATIC_TEXT &&
			it->getType() != gui::EGUIET_TAB_CONTROL) {
			Environment->setFocus(it);
			return;
		}
	}

	// 6. first child element
	if (children.empty())
		Environment->setFocus(this);
	else
		Environment->setFocus(*(children.begin()));
}

GUITable* GUIFormSpecMenu::getTable(const std::string &tablename)
{
	for (auto &table : m_tables) {
		if (tablename == table.first.fname)
			return table.second;
	}
	return 0;
}

std::vector<std::string>* GUIFormSpecMenu::getDropDownValues(const std::string &name)
{
	for (auto &dropdown : m_dropdowns) {
		if (name == dropdown.first.fname)
			return &dropdown.second;
	}
	return NULL;
}

v2s32 GUIFormSpecMenu::getElementBasePos(bool absolute,
		const std::vector<std::string> *v_pos)
{
	v2s32 pos = padding;
	if (absolute)
		pos += AbsoluteRect.UpperLeftCorner;

	v2f32 pos_f = v2f32(pos.X, pos.Y) + pos_offset * spacing;
	if (v_pos) {
		pos_f.X += stof((*v_pos)[0]) * spacing.X;
		pos_f.Y += stof((*v_pos)[1]) * spacing.Y;
	}
	return v2s32(pos_f.X, pos_f.Y);
}

v2s32 GUIFormSpecMenu::getRealCoordinateBasePos(bool absolute,
		const std::vector<std::string> &v_pos)
{
	v2f32 pos_f = v2f32(0.0f, 0.0f);

	pos_f.X += stof(v_pos[0]) + pos_offset.X;
	pos_f.Y += stof(v_pos[1]) + pos_offset.Y;

	if (absolute)
		return v2s32(pos_f.X * imgsize.X + AbsoluteRect.UpperLeftCorner.X,
				pos_f.Y * imgsize.Y + AbsoluteRect.UpperLeftCorner.Y);
	return v2s32(pos_f.X * imgsize.X, pos_f.Y * imgsize.Y);
}

v2s32 GUIFormSpecMenu::getRealCoordinateGeometry(const std::vector<std::string> &v_geom)
{
	return v2s32(stof(v_geom[0]) * imgsize.X, stof(v_geom[1]) * imgsize.Y);
}

void GUIFormSpecMenu::parseSize(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,',');

	if (((parts.size() == 2) || parts.size() == 3) ||
		((parts.size() > 3) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		if (parts[1].find(';') != std::string::npos)
			parts[1] = parts[1].substr(0,parts[1].find(';'));

		data->invsize.X = MYMAX(0, stof(parts[0]));
		data->invsize.Y = MYMAX(0, stof(parts[1]));

		lockSize(false);
#ifndef __ANDROID__
		if (parts.size() == 3) {
			if (parts[2] == "true") {
				lockSize(true,v2u32(800,600));
			}
		}
#endif
		data->explicit_size = true;
		return;
	}
	errorstream<< "Invalid size element (" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseContainer(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element, ',');

	if (parts.size() >= 2) {
		if (parts[1].find(';') != std::string::npos)
			parts[1] = parts[1].substr(0, parts[1].find(';'));

		container_stack.push(pos_offset);
		pos_offset.X += stof(parts[0]);
		pos_offset.Y += stof(parts[1]);
		return;
	}
	errorstream<< "Invalid container start element (" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseContainerEnd(parserData* data)
{
	if (container_stack.empty()) {
		errorstream<< "Invalid container end element, no matching container start element"  << std::endl;
	} else {
		pos_offset = container_stack.top();
		container_stack.pop();
	}
}

void GUIFormSpecMenu::parseList(parserData* data, const std::string &element)
{
	if (m_client == 0) {
		warningstream<<"invalid use of 'list' with m_client==0"<<std::endl;
		return;
	}

	std::vector<std::string> parts = split(element,';');

	if (((parts.size() == 4) || (parts.size() == 5)) ||
		((parts.size() > 5) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::string location = parts[0];
		std::string listname = parts[1];
		std::vector<std::string> v_pos  = split(parts[2],',');
		std::vector<std::string> v_geom = split(parts[3],',');
		std::string startindex;
		if (parts.size() == 5)
			startindex = parts[4];

		MY_CHECKPOS("list",2);
		MY_CHECKGEOM("list",3);

		InventoryLocation loc;

		if (location == "context" || location == "current_name")
			loc = m_current_inventory_location;
		else
			loc.deSerialize(location);

		v2s32 pos;
		v2s32 geom;

		if (data->real_coordinates)
			pos = getRealCoordinateBasePos(true, v_pos);
		else
			pos = getElementBasePos(true, &v_pos);

		geom.X = stoi(v_geom[0]);
		geom.Y = stoi(v_geom[1]);

		s32 start_i = 0;
		if (!startindex.empty())
			start_i = stoi(startindex);

		if (geom.X < 0 || geom.Y < 0 || start_i < 0) {
			errorstream<< "Invalid list element: '" << element << "'"  << std::endl;
			return;
		}

		if(!data->explicit_size)
			warningstream<<"invalid use of list without a size[] element"<<std::endl;
		m_inventorylists.emplace_back(loc, listname, pos, geom, start_i, data->real_coordinates);
		return;
	}
	errorstream<< "Invalid list element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseListRing(parserData* data, const std::string &element)
{
	if (m_client == 0) {
		errorstream << "WARNING: invalid use of 'listring' with m_client==0" << std::endl;
		return;
	}

	std::vector<std::string> parts = split(element, ';');

	if (parts.size() == 2) {
		std::string location = parts[0];
		std::string listname = parts[1];

		InventoryLocation loc;

		if (location == "context" || location == "current_name")
			loc = m_current_inventory_location;
		else
			loc.deSerialize(location);

		m_inventory_rings.emplace_back(loc, listname);
		return;
	}

	if (element.empty() && m_inventorylists.size() > 1) {
		size_t siz = m_inventorylists.size();
		// insert the last two inv list elements into the list ring
		const ListDrawSpec &spa = m_inventorylists[siz - 2];
		const ListDrawSpec &spb = m_inventorylists[siz - 1];
		m_inventory_rings.emplace_back(spa.inventoryloc, spa.listname);
		m_inventory_rings.emplace_back(spb.inventoryloc, spb.listname);
		return;
	}

	errorstream<< "Invalid list ring element(" << parts.size() << ", "
		<< m_inventorylists.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseCheckbox(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if (((parts.size() >= 3) && (parts.size() <= 4)) ||
		((parts.size() > 4) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');
		std::string name = parts[1];
		std::string label = parts[2];
		std::string selected;

		if (parts.size() >= 4)
			selected = parts[3];

		MY_CHECKPOS("checkbox",0);

		bool fselected = false;

		if (selected == "true")
			fselected = true;

		std::wstring wlabel = translate_string(utf8_to_wide(unescape_string(label)));
		const core::dimension2d<u32> label_size = m_font->getDimension(wlabel.c_str());
		s32 cb_size = Environment->getSkin()->getSize(gui::EGDS_CHECK_BOX_WIDTH);
		s32 y_center = (std::max(label_size.Height, (u32)cb_size) + 1) / 2;

		v2s32 pos;
		core::rect<s32> rect;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(false, v_pos);

			rect = core::rect<s32>(
					pos.X,
					pos.Y - y_center,
					pos.X + label_size.Width + cb_size + 7,
					pos.Y + y_center
				);
		} else {
			pos = getElementBasePos(false, &v_pos);
			rect = core::rect<s32>(
					pos.X,
					pos.Y + imgsize.Y / 2 - y_center,
					pos.X + label_size.Width + cb_size + 7,
					pos.Y + imgsize.Y / 2 + y_center
				);
		}

		FieldSpec spec(
				name,
				wlabel, //Needed for displaying text on MSVC
				wlabel,
				258+m_fields.size()
			);

		spec.ftype = f_CheckBox;

		gui::IGUICheckBox* e = Environment->addCheckBox(fselected, rect, this,
					spec.fid, spec.flabel.c_str());

		auto style = getStyleForElement("checkbox", name);
		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));

		if (spec.fname == data->focused_fieldname) {
			Environment->setFocus(e);
		}

		m_checkboxes.emplace_back(spec,e);
		m_fields.push_back(spec);
		return;
	}
	errorstream<< "Invalid checkbox element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseScrollBar(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if (parts.size() >= 5) {
		std::vector<std::string> v_pos = split(parts[0],',');
		std::vector<std::string> v_geom = split(parts[1],',');
		std::string name = parts[3];
		std::string value = parts[4];

		MY_CHECKPOS("scrollbar",0);
		MY_CHECKGEOM("scrollbar",1);

		v2s32 pos;
		v2s32 dim;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(false, v_pos);
			dim = getRealCoordinateGeometry(v_geom);
		} else {
			pos = getElementBasePos(false, &v_pos);
			dim.X = stof(v_geom[0]) * spacing.X;
			dim.Y = stof(v_geom[1]) * spacing.Y;
		}

		core::rect<s32> rect =
				core::rect<s32>(pos.X, pos.Y, pos.X + dim.X, pos.Y + dim.Y);

		FieldSpec spec(
				name,
				L"",
				L"",
				258+m_fields.size()
			);

		bool is_horizontal = true;

		if (parts[2] == "vertical")
			is_horizontal = false;

		spec.ftype = f_ScrollBar;
		spec.send  = true;
		gui::IGUIScrollBar* e =
				Environment->addScrollBar(is_horizontal,rect,this,spec.fid);

		auto style = getStyleForElement("scrollbar", name);
		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));

		e->setMax(1000);
		e->setMin(0);
		e->setPos(stoi(parts[4]));
		e->setSmallStep(10);
		e->setLargeStep(100);

		m_scrollbars.emplace_back(spec,e);
		m_fields.push_back(spec);
		return;
	}
	errorstream<< "Invalid scrollbar element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseImage(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if ((parts.size() == 3) ||
		((parts.size() > 3) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');
		std::vector<std::string> v_geom = split(parts[1],',');
		std::string name = unescape_string(parts[2]);

		MY_CHECKPOS("image", 0);
		MY_CHECKGEOM("image", 1);

		v2s32 pos;
		v2s32 geom;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(true, v_pos);
			geom = getRealCoordinateGeometry(v_geom);
		} else {
			pos = getElementBasePos(true, &v_pos);
			geom.X = stof(v_geom[0]) * (float)imgsize.X;
			geom.Y = stof(v_geom[1]) * (float)imgsize.Y;
		}

		if (!data->explicit_size)
			warningstream<<"invalid use of image without a size[] element"<<std::endl;
		m_images.emplace_back(name, pos, geom);
		return;
	}

	if (parts.size() == 2) {
		std::vector<std::string> v_pos = split(parts[0],',');
		std::string name = unescape_string(parts[1]);

		MY_CHECKPOS("image", 0);

		v2s32 pos = getElementBasePos(true, &v_pos);

		if (!data->explicit_size)
			warningstream<<"invalid use of image without a size[] element"<<std::endl;
		m_images.emplace_back(name, pos);
		return;
	}
	errorstream<< "Invalid image element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseItemImage(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if ((parts.size() == 3) ||
		((parts.size() > 3) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');
		std::vector<std::string> v_geom = split(parts[1],',');
		std::string name = parts[2];

		MY_CHECKPOS("itemimage",0);
		MY_CHECKGEOM("itemimage",1);

		v2s32 pos;
		v2s32 geom;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(true, v_pos);
			geom = getRealCoordinateGeometry(v_geom);
		} else {
			pos = getElementBasePos(true, &v_pos);
			geom.X = stof(v_geom[0]) * (float)imgsize.X;
			geom.Y = stof(v_geom[1]) * (float)imgsize.Y;
		}

		if(!data->explicit_size)
			warningstream<<"invalid use of item_image without a size[] element"<<std::endl;
		m_itemimages.emplace_back("", name, pos, geom);
		return;
	}
	errorstream<< "Invalid ItemImage element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseButton(parserData* data, const std::string &element,
		const std::string &type)
{
	std::vector<std::string> parts = split(element,';');

	if ((parts.size() == 4) ||
		((parts.size() > 4) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');
		std::vector<std::string> v_geom = split(parts[1],',');
		std::string name = parts[2];
		std::string label = parts[3];

		MY_CHECKPOS("button",0);
		MY_CHECKGEOM("button",1);

		v2s32 pos;
		v2s32 geom;
		core::rect<s32> rect;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(false, v_pos);
			geom = getRealCoordinateGeometry(v_geom);
			rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X,
				pos.Y+geom.Y);
		} else {
			pos = getElementBasePos(false, &v_pos);
			geom.X = (stof(v_geom[0]) * spacing.X) - (spacing.X - imgsize.X);
			pos.Y += (stof(v_geom[1]) * (float)imgsize.Y)/2;

			rect = core::rect<s32>(pos.X, pos.Y - m_btn_height,
						pos.X + geom.X, pos.Y + m_btn_height);
		}

		if(!data->explicit_size)
			warningstream<<"invalid use of button without a size[] element"<<std::endl;

		std::wstring wlabel = translate_string(utf8_to_wide(unescape_string(label)));

		FieldSpec spec(
			name,
			wlabel,
			L"",
			258+m_fields.size()
		);
		spec.ftype = f_Button;
		if(type == "button_exit")
			spec.is_exit = true;

		GUIButton *e = GUIButton::addButton(Environment, rect, this, spec.fid, spec.flabel.c_str());

		auto style = getStyleForElement(type, name, (type != "button") ? "button" : "");
		if (style.isNotDefault(StyleSpec::BGCOLOR)) {
			e->setColor(style.getColor(StyleSpec::BGCOLOR));
		}
		if (style.isNotDefault(StyleSpec::BGCOLOR_HOVERED)) {
			e->setHoveredColor(style.getColor(StyleSpec::BGCOLOR_HOVERED));
		}
		if (style.isNotDefault(StyleSpec::BGCOLOR_PRESSED)) {
			e->setPressedColor(style.getColor(StyleSpec::BGCOLOR_PRESSED));
		}

		if (style.isNotDefault(StyleSpec::TEXTCOLOR)) {
			e->setOverrideColor(style.getColor(StyleSpec::TEXTCOLOR));
		}
		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
		e->setDrawBorder(style.getBool(StyleSpec::BORDER, true));

		if (style.isNotDefault(StyleSpec::BGIMG)) {
			std::string image_name = style.get(StyleSpec::BGIMG, "");
			std::string hovered_image_name = style.get(StyleSpec::BGIMG_HOVERED, "");
			std::string pressed_image_name = style.get(StyleSpec::BGIMG_PRESSED, "");

			video::ITexture *texture = 0;
			video::ITexture *hovered_texture = 0;
			video::ITexture *pressed_texture = 0;
			texture = m_tsrc->getTexture(image_name);
			if (!hovered_image_name.empty())
				hovered_texture = m_tsrc->getTexture(hovered_image_name);
			else
				hovered_texture = texture;
			if (!pressed_image_name.empty())
				pressed_texture = m_tsrc->getTexture(pressed_image_name);
			else
				pressed_texture = texture;

			e->setUseAlphaChannel(style.getBool(StyleSpec::ALPHA, true));
			e->setImage(guiScalingImageButton(
					Environment->getVideoDriver(), texture, geom.X, geom.Y));
			e->setHoveredImage(guiScalingImageButton(
					Environment->getVideoDriver(), hovered_texture, geom.X, geom.Y));
			e->setPressedImage(guiScalingImageButton(
					Environment->getVideoDriver(), pressed_texture, geom.X, geom.Y));
			e->setScaleImage(true);
		}

		if (spec.fname == data->focused_fieldname) {
			Environment->setFocus(e);
		}

		m_fields.push_back(spec);
		return;
	}
	errorstream<< "Invalid button element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseBackground(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if ((parts.size() >= 3 && parts.size() <= 5) ||
			(parts.size() > 5 && m_formspec_version > FORMSPEC_API_VERSION)) {
		std::vector<std::string> v_pos = split(parts[0],',');
		std::vector<std::string> v_geom = split(parts[1],',');
		std::string name = unescape_string(parts[2]);

		MY_CHECKPOS("background",0);
		MY_CHECKGEOM("background",1);

		v2s32 pos;
		v2s32 geom;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(true, v_pos);
			geom = getRealCoordinateGeometry(v_geom);
		} else {
			pos = getElementBasePos(true, &v_pos);
			pos.X -= (spacing.X - (float)imgsize.X) / 2;
			pos.Y -= (spacing.Y - (float)imgsize.Y) / 2;

			geom.X = stof(v_geom[0]) * spacing.X;
			geom.Y = stof(v_geom[1]) * spacing.Y;
		}

		bool clip = false;
		if (parts.size() >= 4 && is_yes(parts[3])) {
			if (data->real_coordinates) {
				pos = getRealCoordinateBasePos(false, v_pos) * -1;
				geom = v2s32(0, 0);
			} else {
				pos.X = stoi(v_pos[0]); //acts as offset
				pos.Y = stoi(v_pos[1]);
			}
			clip = true;
		}

		core::rect<s32> middle;
		if (parts.size() >= 5) {
			std::vector<std::string> v_middle = split(parts[4], ',');
			if (v_middle.size() == 1) {
				s32 x = stoi(v_middle[0]);
				middle.UpperLeftCorner = core::vector2di(x, x);
				middle.LowerRightCorner = core::vector2di(-x, -x);
			} else if (v_middle.size() == 2) {
				s32 x = stoi(v_middle[0]);
				s32 y =	stoi(v_middle[1]);
				middle.UpperLeftCorner = core::vector2di(x, y);
				middle.LowerRightCorner = core::vector2di(-x, -y);
				// `-x` is interpreted as `w - x`
			} else if (v_middle.size() == 4) {
				middle.UpperLeftCorner = core::vector2di(stoi(v_middle[0]), stoi(v_middle[1]));
				middle.LowerRightCorner = core::vector2di(stoi(v_middle[2]), stoi(v_middle[3]));
			} else {
				warningstream << "Invalid rectangle given to middle param of background[] element" << std::endl;
			}
		}

		if (!data->explicit_size && !clip)
			warningstream << "invalid use of unclipped background without a size[] element" << std::endl;

		m_backgrounds.emplace_back(name, pos, geom, middle, clip);

		return;
	}
	errorstream<< "Invalid background element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseTableOptions(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	data->table_options.clear();
	for (const std::string &part : parts) {
		// Parse table option
		std::string opt = unescape_string(part);
		data->table_options.push_back(GUITable::splitOption(opt));
	}
}

void GUIFormSpecMenu::parseTableColumns(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	data->table_columns.clear();
	for (const std::string &part : parts) {
		std::vector<std::string> col_parts = split(part,',');
		GUITable::TableColumn column;
		// Parse column type
		if (!col_parts.empty())
			column.type = col_parts[0];
		// Parse column options
		for (size_t j = 1; j < col_parts.size(); ++j) {
			std::string opt = unescape_string(col_parts[j]);
			column.options.push_back(GUITable::splitOption(opt));
		}
		data->table_columns.push_back(column);
	}
}

void GUIFormSpecMenu::parseTable(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if (((parts.size() == 4) || (parts.size() == 5)) ||
		((parts.size() > 5) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');
		std::vector<std::string> v_geom = split(parts[1],',');
		std::string name = parts[2];
		std::vector<std::string> items = split(parts[3],',');
		std::string str_initial_selection;
		std::string str_transparent = "false";

		if (parts.size() >= 5)
			str_initial_selection = parts[4];

		MY_CHECKPOS("table",0);
		MY_CHECKGEOM("table",1);

		v2s32 pos;
		v2s32 geom;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(false, v_pos);
			geom = getRealCoordinateGeometry(v_geom);
		} else {
			pos = getElementBasePos(false, &v_pos);
			geom.X = stof(v_geom[0]) * spacing.X;
			geom.Y = stof(v_geom[1]) * spacing.Y;
		}

		core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X, pos.Y+geom.Y);

		FieldSpec spec(
			name,
			L"",
			L"",
			258+m_fields.size()
		);

		spec.ftype = f_Table;

		for (std::string &item : items) {
			item = wide_to_utf8(unescape_translate(utf8_to_wide(unescape_string(item))));
		}

		//now really show table
		GUITable *e = new GUITable(Environment, this, spec.fid, rect,
				m_tsrc);

		if (spec.fname == data->focused_fieldname) {
			Environment->setFocus(e);
		}

		e->setTable(data->table_options, data->table_columns, items);

		if (data->table_dyndata.find(name) != data->table_dyndata.end()) {
			e->setDynamicData(data->table_dyndata[name]);
		}

		if (!str_initial_selection.empty() && str_initial_selection != "0")
			e->setSelected(stoi(str_initial_selection));

		auto style = getStyleForElement("table", name);
		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));

		m_tables.emplace_back(spec, e);
		m_fields.push_back(spec);
		return;
	}
	errorstream<< "Invalid table element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseTextList(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if (((parts.size() == 4) || (parts.size() == 5) || (parts.size() == 6)) ||
		((parts.size() > 6) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');
		std::vector<std::string> v_geom = split(parts[1],',');
		std::string name = parts[2];
		std::vector<std::string> items = split(parts[3],',');
		std::string str_initial_selection;
		std::string str_transparent = "false";

		if (parts.size() >= 5)
			str_initial_selection = parts[4];

		if (parts.size() >= 6)
			str_transparent = parts[5];

		MY_CHECKPOS("textlist",0);
		MY_CHECKGEOM("textlist",1);

		v2s32 pos;
		v2s32 geom;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(false, v_pos);
			geom = getRealCoordinateGeometry(v_geom);
		} else {
			pos = getElementBasePos(false, &v_pos);
			geom.X = stof(v_geom[0]) * spacing.X;
			geom.Y = stof(v_geom[1]) * spacing.Y;
		}

		core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X, pos.Y+geom.Y);

		FieldSpec spec(
			name,
			L"",
			L"",
			258+m_fields.size()
		);

		spec.ftype = f_Table;

		for (std::string &item : items) {
			item = wide_to_utf8(unescape_translate(utf8_to_wide(unescape_string(item))));
		}

		//now really show list
		GUITable *e = new GUITable(Environment, this, spec.fid, rect,
				m_tsrc);

		if (spec.fname == data->focused_fieldname) {
			Environment->setFocus(e);
		}

		e->setTextList(items, is_yes(str_transparent));

		if (data->table_dyndata.find(name) != data->table_dyndata.end()) {
			e->setDynamicData(data->table_dyndata[name]);
		}

		if (!str_initial_selection.empty() && str_initial_selection != "0")
			e->setSelected(stoi(str_initial_selection));

		auto style = getStyleForElement("textlist", name);
		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));

		m_tables.emplace_back(spec, e);
		m_fields.push_back(spec);
		return;
	}
	errorstream<< "Invalid textlist element(" << parts.size() << "): '" << element << "'"  << std::endl;
}


void GUIFormSpecMenu::parseDropDown(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if ((parts.size() == 5) ||
		((parts.size() > 5) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');
		std::string name = parts[2];
		std::vector<std::string> items = split(parts[3],',');
		std::string str_initial_selection;
		str_initial_selection = parts[4];

		MY_CHECKPOS("dropdown",0);

		v2s32 pos;
		v2s32 geom;
		core::rect<s32> rect;

		if (data->real_coordinates) {
			std::vector<std::string> v_geom = split(parts[1],',');

			if (v_geom.size() == 1)
				v_geom.emplace_back("1");

			MY_CHECKGEOM("dropdown",1);

			pos = getRealCoordinateBasePos(false, v_pos);
			geom = getRealCoordinateGeometry(v_geom);
			rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X, pos.Y+geom.Y);
		} else {
			pos = getElementBasePos(false, &v_pos);

			s32 width = stof(parts[1]) * spacing.Y;

			rect = core::rect<s32>(pos.X, pos.Y,
					pos.X + width, pos.Y + (m_btn_height * 2));
		}

		FieldSpec spec(
			name,
			L"",
			L"",
			258+m_fields.size()
		);

		spec.ftype = f_DropDown;
		spec.send = true;

		//now really show list
		gui::IGUIComboBox *e = Environment->addComboBox(rect, this,spec.fid);

		if (spec.fname == data->focused_fieldname) {
			Environment->setFocus(e);
		}

		for (const std::string &item : items) {
			e->addItem(unescape_translate(unescape_string(
				utf8_to_wide(item))).c_str());
		}

		if (!str_initial_selection.empty())
			e->setSelected(stoi(str_initial_selection)-1);

		auto style = getStyleForElement("dropdown", name);
		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));

		m_fields.push_back(spec);

		m_dropdowns.emplace_back(spec, std::vector<std::string>());
		std::vector<std::string> &values = m_dropdowns.back().second;
		for (const std::string &item : items) {
			values.push_back(unescape_string(item));
		}

		return;
	}
	errorstream << "Invalid dropdown element(" << parts.size() << "): '"
				<< element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseFieldCloseOnEnter(parserData *data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');
	if (parts.size() == 2 ||
			(parts.size() > 2 && m_formspec_version > FORMSPEC_API_VERSION)) {
		field_close_on_enter[parts[0]] = is_yes(parts[1]);
	}
}

void GUIFormSpecMenu::parsePwdField(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if ((parts.size() == 4) || (parts.size() == 5) ||
		((parts.size() > 5) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');
		std::vector<std::string> v_geom = split(parts[1],',');
		std::string name = parts[2];
		std::string label = parts[3];

		MY_CHECKPOS("pwdfield",0);
		MY_CHECKGEOM("pwdfield",1);

		v2s32 pos;
		v2s32 geom;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(false, v_pos);
			geom = getRealCoordinateGeometry(v_geom);
		} else {
			pos = getElementBasePos(false, &v_pos);
			pos -= padding;

			geom.X = (stof(v_geom[0]) * spacing.X) - (spacing.X - imgsize.X);

			pos.Y += (stof(v_geom[1]) * (float)imgsize.Y)/2;
			pos.Y -= m_btn_height;
			geom.Y = m_btn_height*2;
		}

		core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X, pos.Y+geom.Y);

		std::wstring wlabel = translate_string(utf8_to_wide(unescape_string(label)));

		FieldSpec spec(
			name,
			wlabel,
			L"",
			258+m_fields.size()
			);

		spec.send = true;
		gui::IGUIEditBox * e = Environment->addEditBox(0, rect, true, this, spec.fid);

		if (spec.fname == data->focused_fieldname) {
			Environment->setFocus(e);
		}

		if (label.length() >= 1) {
			int font_height = g_fontengine->getTextHeight();
			rect.UpperLeftCorner.Y -= font_height;
			rect.LowerRightCorner.Y = rect.UpperLeftCorner.Y + font_height;
			gui::StaticText::add(Environment, spec.flabel.c_str(), rect, false, true,
				this, 0);
		}

		e->setPasswordBox(true,L'*');

		auto style = getStyleForElement("pwdfield", name, "field");
		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
		e->setDrawBorder(style.getBool(StyleSpec::BORDER, true));
		e->setOverrideColor(style.getColor(StyleSpec::TEXTCOLOR, video::SColor(0xFFFFFFFF)));

		irr::SEvent evt;
		evt.EventType            = EET_KEY_INPUT_EVENT;
		evt.KeyInput.Key         = KEY_END;
		evt.KeyInput.Char        = 0;
		evt.KeyInput.Control     = false;
		evt.KeyInput.Shift       = false;
		evt.KeyInput.PressedDown = true;
		e->OnEvent(evt);

		if (parts.size() >= 5) {
			// TODO: remove after 2016-11-03
			warningstream << "pwdfield: use field_close_on_enter[name, enabled]" <<
					" instead of the 5th param" << std::endl;
			field_close_on_enter[name] = is_yes(parts[4]);
		}

		m_fields.push_back(spec);
		return;
	}
	errorstream<< "Invalid pwdfield element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::createTextField(parserData *data, FieldSpec &spec,
	core::rect<s32> &rect, bool is_multiline)
{
	bool is_editable = !spec.fname.empty();
	if (!is_editable && !is_multiline) {
		// spec field id to 0, this stops submit searching for a value that isn't there
		gui::StaticText::add(Environment, spec.flabel.c_str(), rect, false, true,
			this, spec.fid);
		return;
	}

	if (is_editable) {
		spec.send = true;
	} else if (is_multiline &&
			spec.fdefault.empty() && !spec.flabel.empty()) {
		// Multiline textareas: swap default and label for backwards compat
		spec.flabel.swap(spec.fdefault);
	}

	gui::IGUIEditBox *e = nullptr;
	static constexpr bool use_intl_edit_box = USE_FREETYPE &&
		IRRLICHT_VERSION_MAJOR == 1 && IRRLICHT_VERSION_MINOR < 9;

	if (use_intl_edit_box && g_settings->getBool("freetype")) {
		e = new gui::intlGUIEditBox(spec.fdefault.c_str(),
			true, Environment, this, spec.fid, rect, is_editable, is_multiline);
		e->drop();
	} else {
		if (is_multiline) {
			e = new GUIEditBoxWithScrollBar(spec.fdefault.c_str(), true,
				Environment, this, spec.fid, rect, is_editable, true);
			e->drop();
		} else if (is_editable) {
			e = Environment->addEditBox(spec.fdefault.c_str(), rect, true,
				this, spec.fid);
		}
	}

	if (e) {
		if (is_editable && spec.fname == data->focused_fieldname)
			Environment->setFocus(e);

		if (is_multiline) {
			e->setMultiLine(true);
			e->setWordWrap(true);
			e->setTextAlignment(gui::EGUIA_UPPERLEFT, gui::EGUIA_UPPERLEFT);
		} else {
			irr::SEvent evt;
			evt.EventType            = EET_KEY_INPUT_EVENT;
			evt.KeyInput.Key         = KEY_END;
			evt.KeyInput.Char        = 0;
			evt.KeyInput.Control     = 0;
			evt.KeyInput.Shift       = 0;
			evt.KeyInput.PressedDown = true;
			e->OnEvent(evt);
		}

		auto style = getStyleForElement(is_multiline ? "textarea" : "field", spec.fname);
		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
		e->setDrawBorder(style.getBool(StyleSpec::BORDER, true));
		e->setOverrideColor(style.getColor(StyleSpec::TEXTCOLOR, video::SColor(0xFFFFFFFF)));
		if (style.get(StyleSpec::BGCOLOR, "") == "transparent") {
			e->setDrawBackground(false);
		}
	}

	if (!spec.flabel.empty()) {
		int font_height = g_fontengine->getTextHeight();
		rect.UpperLeftCorner.Y -= font_height;
		rect.LowerRightCorner.Y = rect.UpperLeftCorner.Y + font_height;
		gui::StaticText::add(Environment, spec.flabel.c_str(), rect, false, true,
			this, 0);
	}
}

void GUIFormSpecMenu::parseSimpleField(parserData* data,
		std::vector<std::string> &parts)
{
	std::string name = parts[0];
	std::string label = parts[1];
	std::string default_val = parts[2];

	core::rect<s32> rect;

	if(data->explicit_size)
		warningstream<<"invalid use of unpositioned \"field\" in inventory"<<std::endl;

	v2s32 pos = getElementBasePos(false, nullptr);
	pos.Y = ((m_fields.size()+2)*60);
	v2s32 size = DesiredRect.getSize();

	rect = core::rect<s32>(size.X / 2 - 150, pos.Y,
			(size.X / 2 - 150) + 300, pos.Y + (m_btn_height*2));


	if(m_form_src)
		default_val = m_form_src->resolveText(default_val);


	std::wstring wlabel = translate_string(utf8_to_wide(unescape_string(label)));

	FieldSpec spec(
		name,
		wlabel,
		utf8_to_wide(unescape_string(default_val)),
		258+m_fields.size()
	);

	createTextField(data, spec, rect, false);

	if (parts.size() >= 4) {
		// TODO: remove after 2016-11-03
		warningstream << "field/simple: use field_close_on_enter[name, enabled]" <<
				" instead of the 4th param" << std::endl;
		field_close_on_enter[name] = is_yes(parts[3]);
	}

	m_fields.push_back(spec);
}

void GUIFormSpecMenu::parseTextArea(parserData* data, std::vector<std::string>& parts,
		const std::string &type)
{
	std::vector<std::string> v_pos = split(parts[0],',');
	std::vector<std::string> v_geom = split(parts[1],',');
	std::string name = parts[2];
	std::string label = parts[3];
	std::string default_val = parts[4];

	MY_CHECKPOS(type,0);
	MY_CHECKGEOM(type,1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(false, v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(false, &v_pos);
		pos -= padding;

		geom.X = (stof(v_geom[0]) * spacing.X) - (spacing.X - imgsize.X);

		if (type == "textarea")
		{
			geom.Y = (stof(v_geom[1]) * (float)imgsize.Y) - (spacing.Y-imgsize.Y);
			pos.Y += m_btn_height;
		}
		else
		{
			pos.Y += (stof(v_geom[1]) * (float)imgsize.Y)/2;
			pos.Y -= m_btn_height;
			geom.Y = m_btn_height*2;
		}
	}

	core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X, pos.Y+geom.Y);

	if(!data->explicit_size)
		warningstream<<"invalid use of positioned "<<type<<" without a size[] element"<<std::endl;

	if(m_form_src)
		default_val = m_form_src->resolveText(default_val);


	std::wstring wlabel = translate_string(utf8_to_wide(unescape_string(label)));

	FieldSpec spec(
		name,
		wlabel,
		utf8_to_wide(unescape_string(default_val)),
		258+m_fields.size()
	);

	createTextField(data, spec, rect, type == "textarea");

	if (parts.size() >= 6) {
		// TODO: remove after 2016-11-03
		warningstream << "field/textarea: use field_close_on_enter[name, enabled]" <<
				" instead of the 6th param" << std::endl;
		field_close_on_enter[name] = is_yes(parts[5]);
	}

	m_fields.push_back(spec);
}

void GUIFormSpecMenu::parseField(parserData* data, const std::string &element,
		const std::string &type)
{
	std::vector<std::string> parts = split(element,';');

	if (parts.size() == 3 || parts.size() == 4) {
		parseSimpleField(data,parts);
		return;
	}

	if ((parts.size() == 5) || (parts.size() == 6) ||
		((parts.size() > 6) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		parseTextArea(data,parts,type);
		return;
	}
	errorstream<< "Invalid field element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseHyperText(parserData *data, const std::string &element)
{
	std::vector<std::string> parts = split(element, ';');

	if (parts.size() != 4 && m_formspec_version < FORMSPEC_API_VERSION) {
		errorstream << "Invalid text element(" << parts.size() << "): '" << element << "'"  << std::endl;
		return;
	}

	std::vector<std::string> v_pos = split(parts[0], ',');
	std::vector<std::string> v_geom = split(parts[1], ',');
	std::string name = parts[2];
	std::string text = parts[3];

	MY_CHECKPOS("hypertext", 0);
	MY_CHECKGEOM("hypertext", 1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(false, v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(false, &v_pos);
		pos -= padding;

		pos.X += stof(v_pos[0]) * spacing.X;
		pos.Y += stof(v_pos[1]) * spacing.Y + (m_btn_height * 2);

		geom.X = (stof(v_geom[0]) * spacing.X) - (spacing.X - imgsize.X);
		geom.Y = (stof(v_geom[1]) * imgsize.Y) - (spacing.Y - imgsize.Y);
	}

	core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X + geom.X, pos.Y + geom.Y);

	if(m_form_src)
		text = m_form_src->resolveText(text);

	FieldSpec spec(
		name,
		utf8_to_wide(unescape_string(text)),
		L"",
		258 + m_fields.size()
	);

	spec.ftype = f_Unknown;
	new GUIHyperText(
		spec.flabel.c_str(), Environment, this, spec.fid, rect, m_client, m_tsrc);

	m_fields.push_back(spec);
}

void GUIFormSpecMenu::parseLabel(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if ((parts.size() == 2) ||
		((parts.size() > 2) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');
		std::string text = parts[1];

		MY_CHECKPOS("label",0);

		if(!data->explicit_size)
			warningstream<<"invalid use of label without a size[] element"<<std::endl;

		std::vector<std::string> lines = split(text, '\n');

		for (unsigned int i = 0; i != lines.size(); i++) {
			std::wstring wlabel_colors = translate_string(
				utf8_to_wide(unescape_string(lines[i])));
			// Without color escapes to get the font dimensions
			std::wstring wlabel_plain = unescape_enriched(wlabel_colors);

			core::rect<s32> rect;

			if (data->real_coordinates) {
				// Lines are spaced at the distance of 1/2 imgsize.
				// This alows lines that line up with the new elements
				// easily without sacrificing good line distance.  If
				// it was one whole imgsize, it would have too much
				// spacing.
				v2s32 pos = getRealCoordinateBasePos(false, v_pos);

				// Labels are positioned by their center, not their top.
				pos.Y += (((float) imgsize.Y) / -2) + (((float) imgsize.Y) * i / 2);

				rect = core::rect<s32>(
					pos.X, pos.Y,
					pos.X + m_font->getDimension(wlabel_plain.c_str()).Width,
					pos.Y + imgsize.Y);

			} else {
				// Lines are spaced at the nominal distance of
				// 2/5 inventory slot, even if the font doesn't
				// quite match that.  This provides consistent
				// form layout, at the expense of sometimes
				// having sub-optimal spacing for the font.
				// We multiply by 2 and then divide by 5, rather
				// than multiply by 0.4, to get exact results
				// in the integer cases: 0.4 is not exactly
				// representable in binary floating point.

				v2s32 pos = getElementBasePos(false, nullptr);
				pos.X += stof(v_pos[0]) * spacing.X;
				pos.Y += (stof(v_pos[1]) + 7.0f / 30.0f) * spacing.Y;

				pos.Y += ((float) i) * spacing.Y * 2.0 / 5.0;

				rect = core::rect<s32>(
					pos.X, pos.Y - m_btn_height,
					pos.X + m_font->getDimension(wlabel_plain.c_str()).Width,
					pos.Y + m_btn_height);
			}

			FieldSpec spec(
				"",
				wlabel_colors,
				L"",
				258+m_fields.size()
			);
			gui::IGUIStaticText *e = gui::StaticText::add(Environment,
				spec.flabel.c_str(), rect, false, false, this, spec.fid);
			e->setTextAlignment(gui::EGUIA_UPPERLEFT, gui::EGUIA_CENTER);

			auto style = getStyleForElement("label", spec.fname);
			e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
			e->setOverrideColor(style.getColor(StyleSpec::TEXTCOLOR, video::SColor(0xFFFFFFFF)));

			m_fields.push_back(spec);
		}

		return;
	}
	errorstream << "Invalid label element(" << parts.size() << "): '" << element
		<< "'"  << std::endl;
}

void GUIFormSpecMenu::parseVertLabel(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if ((parts.size() == 2) ||
		((parts.size() > 2) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');
		std::wstring text = unescape_translate(
			unescape_string(utf8_to_wide(parts[1])));

		MY_CHECKPOS("vertlabel",1);

		v2s32 pos;
		core::rect<s32> rect;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(false, v_pos);

			// Vertlabels are positioned by center, not left.
			pos.X -= imgsize.X / 2;

			// We use text.length + 1 because without it, the rect
			// isn't quite tall enough and cuts off the text.
			rect = core::rect<s32>(pos.X, pos.Y,
				pos.X + imgsize.X,
				pos.Y + font_line_height(m_font) *
				(text.length() + 1));

		} else {
			pos = getElementBasePos(false, &v_pos);

			// As above, the length must be one longer. The width of
			// the rect (15 pixels) seems rather arbitrary, but
			// changing it might break something.
			rect = core::rect<s32>(
				pos.X, pos.Y+((imgsize.Y/2) - m_btn_height),
				pos.X+15, pos.Y +
					font_line_height(m_font) *
					(text.length() + 1) +
					((imgsize.Y/2) - m_btn_height));
		}

		if(!data->explicit_size)
			warningstream<<"invalid use of label without a size[] element"<<std::endl;

		std::wstring label;

		for (wchar_t i : text) {
			label += i;
			label += L"\n";
		}

		FieldSpec spec(
			"",
			label,
			L"",
			258+m_fields.size()
		);
		gui::IGUIStaticText *e = gui::StaticText::add(Environment, spec.flabel.c_str(),
			rect, false, false, this, spec.fid);
		e->setTextAlignment(gui::EGUIA_CENTER, gui::EGUIA_CENTER);

		auto style = getStyleForElement("vertlabel", spec.fname, "label");
		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
		e->setOverrideColor(style.getColor(StyleSpec::TEXTCOLOR, video::SColor(0xFFFFFFFF)));

		m_fields.push_back(spec);
		return;
	}
	errorstream<< "Invalid vertlabel element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseImageButton(parserData* data, const std::string &element,
		const std::string &type)
{
	std::vector<std::string> parts = split(element,';');

	if ((((parts.size() >= 5) && (parts.size() <= 8)) && (parts.size() != 6)) ||
		((parts.size() > 8) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');
		std::vector<std::string> v_geom = split(parts[1],',');
		std::string image_name = parts[2];
		std::string name = parts[3];
		std::string label = parts[4];

		MY_CHECKPOS("imagebutton",0);
		MY_CHECKGEOM("imagebutton",1);

		bool noclip     = false;
		bool drawborder = true;
		std::string pressed_image_name;

		if (parts.size() >= 7) {
			if (parts[5] == "true")
				noclip = true;
			if (parts[6] == "false")
				drawborder = false;
		}

		if (parts.size() >= 8) {
			pressed_image_name = parts[7];
		}

		v2s32 pos;
		v2s32 geom;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(false, v_pos);
			geom = getRealCoordinateGeometry(v_geom);
		} else {
			pos = getElementBasePos(false, &v_pos);
			geom.X = (stof(v_geom[0]) * spacing.X) - (spacing.X - imgsize.X);
			geom.Y = (stof(v_geom[1]) * spacing.Y) - (spacing.Y - imgsize.Y);
		}

		core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X,
			pos.Y+geom.Y);

		if (!data->explicit_size)
			warningstream<<"invalid use of image_button without a size[] element"<<std::endl;

		image_name = unescape_string(image_name);
		pressed_image_name = unescape_string(pressed_image_name);

		std::wstring wlabel = utf8_to_wide(unescape_string(label));

		FieldSpec spec(
			name,
			wlabel,
			utf8_to_wide(image_name),
			258+m_fields.size()
		);
		spec.ftype = f_Button;
		if (type == "image_button_exit")
			spec.is_exit = true;

		video::ITexture *texture = 0;
		video::ITexture *pressed_texture = 0;
		texture = m_tsrc->getTexture(image_name);
		if (!pressed_image_name.empty())
			pressed_texture = m_tsrc->getTexture(pressed_image_name);
		else
			pressed_texture = texture;

		GUIButton *e = GUIButton::addButton(Environment, rect, this, spec.fid, spec.flabel.c_str());

		if (spec.fname == data->focused_fieldname) {
			Environment->setFocus(e);
		}

		auto style = getStyleForElement("image_button", spec.fname);

		e->setUseAlphaChannel(style.getBool(StyleSpec::ALPHA, true));
		e->setImage(guiScalingImageButton(
			Environment->getVideoDriver(), texture, geom.X, geom.Y));
		e->setPressedImage(guiScalingImageButton(
			Environment->getVideoDriver(), pressed_texture, geom.X, geom.Y));
		e->setScaleImage(true);
		if (parts.size() >= 7) {
			e->setNotClipped(noclip);
			e->setDrawBorder(drawborder);
		} else {
			e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
			e->setDrawBorder(style.getBool(StyleSpec::BORDER, true));
		}

		m_fields.push_back(spec);
		return;
	}

	errorstream<< "Invalid imagebutton element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseTabHeader(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element, ';');

	if (((parts.size() == 4) || (parts.size() == 6)) || (parts.size() == 7 &&
		data->real_coordinates) || ((parts.size() > 6) &&
		(m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');

		// If we're using real coordinates, add an extra field for height.
		// Width is not here because tabs are the width of the text, and
		// there's no reason to change that.
		unsigned int i = 0;
		std::vector<std::string> v_geom = {"1", "0.75"}; // Dummy width and default height
		bool auto_width = true;
		if (parts.size() == 7) {
			i++;

			v_geom = split(parts[1], ',');
			if (v_geom.size() == 1)
				v_geom.insert(v_geom.begin(), "1"); // Dummy value
			else
				auto_width = false;
		}

		std::string name = parts[i+1];
		std::vector<std::string> buttons = split(parts[i+2], ',');
		std::string str_index = parts[i+3];
		bool show_background = true;
		bool show_border = true;
		int tab_index = stoi(str_index) - 1;

		MY_CHECKPOS("tabheader", 0);

		if (parts.size() == 6 + i) {
			if (parts[4+i] == "true")
				show_background = false;
			if (parts[5+i] == "false")
				show_border = false;
		}

		FieldSpec spec(
			name,
			L"",
			L"",
			258+m_fields.size()
		);

		spec.ftype = f_TabHeader;

		v2s32 pos;
		v2s32 geom;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(false, v_pos);

			geom = getRealCoordinateGeometry(v_geom);
			pos.Y -= geom.Y; // TabHeader base pos is the bottom, not the top.
			if (auto_width)
				geom.X = DesiredRect.getWidth(); // Set automatic width

			MY_CHECKGEOM("tabheader", 1);
		} else {
			v2f32 pos_f = pos_offset * spacing;
			pos_f.X += stof(v_pos[0]) * spacing.X;
			pos_f.Y += stof(v_pos[1]) * spacing.Y - m_btn_height * 2;
			pos = v2s32(pos_f.X, pos_f.Y);

			geom.Y = m_btn_height * 2;
			geom.X = DesiredRect.getWidth();
		}

		core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X,
				pos.Y+geom.Y);

		gui::IGUITabControl *e = Environment->addTabControl(rect, this,
				show_background, show_border, spec.fid);
		e->setAlignment(irr::gui::EGUIA_UPPERLEFT, irr::gui::EGUIA_UPPERLEFT,
				irr::gui::EGUIA_UPPERLEFT, irr::gui::EGUIA_LOWERRIGHT);
		e->setTabHeight(geom.Y);

		if (spec.fname == data->focused_fieldname) {
			Environment->setFocus(e);
		}

		auto style = getStyleForElement("tabheader", name);
		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, true));

		for (const std::string &button : buttons) {
			auto tab = e->addTab(unescape_translate(unescape_string(
				utf8_to_wide(button))).c_str(), -1);
			if (style.isNotDefault(StyleSpec::BGCOLOR))
				tab->setBackgroundColor(style.getColor(StyleSpec::BGCOLOR));

			tab->setTextColor(style.getColor(StyleSpec::TEXTCOLOR, video::SColor(0xFFFFFFFF)));
		}

		if ((tab_index >= 0) &&
				(buttons.size() < INT_MAX) &&
				(tab_index < (int) buttons.size()))
			e->setActiveTab(tab_index);

		m_fields.push_back(spec);
		return;
	}
	errorstream << "Invalid TabHeader element(" << parts.size() << "): '"
			<< element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseItemImageButton(parserData* data, const std::string &element)
{

	if (m_client == 0) {
		warningstream << "invalid use of item_image_button with m_client==0"
			<< std::endl;
		return;
	}

	std::vector<std::string> parts = split(element,';');

	if ((parts.size() == 5) ||
		((parts.size() > 5) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');
		std::vector<std::string> v_geom = split(parts[1],',');
		std::string item_name = parts[2];
		std::string name = parts[3];
		std::string label = parts[4];

		label = unescape_string(label);
		item_name = unescape_string(item_name);

		MY_CHECKPOS("itemimagebutton",0);
		MY_CHECKGEOM("itemimagebutton",1);

		v2s32 pos;
		v2s32 geom;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(false, v_pos);
			geom = getRealCoordinateGeometry(v_geom);
		} else {
			pos = getElementBasePos(false, &v_pos);
			geom.X = (stof(v_geom[0]) * spacing.X) - (spacing.X - imgsize.X);
			geom.Y = (stof(v_geom[1]) * spacing.Y) - (spacing.Y - imgsize.Y);
		}

		core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X, pos.Y+geom.Y);

		if(!data->explicit_size)
			warningstream<<"invalid use of item_image_button without a size[] element"<<std::endl;

		IItemDefManager *idef = m_client->idef();
		ItemStack item;
		item.deSerialize(item_name, idef);

		m_tooltips[name] =
			TooltipSpec(utf8_to_wide(item.getDefinition(idef).description),
						m_default_tooltip_bgcolor,
						m_default_tooltip_color);

		FieldSpec spec(
			name,
			utf8_to_wide(label),
			utf8_to_wide(item_name),
			258 + m_fields.size()
		);

		gui::IGUIButton *e = GUIButton::addButton(Environment, rect, this, spec.fid, L"");

		auto style = getStyleForElement("item_image_button", spec.fname, "image_button");
		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
		e->setDrawBorder(style.getBool(StyleSpec::BORDER, true));

		if (spec.fname == data->focused_fieldname) {
			Environment->setFocus(e);
		}

		spec.ftype = f_Button;
		rect+=data->basepos-padding;
		spec.rect=rect;
		m_fields.push_back(spec);

		if (data->real_coordinates)
			pos = getRealCoordinateBasePos(true, v_pos);
		else
			pos = getElementBasePos(true, &v_pos);

		m_itemimages.emplace_back("", item_name, e, pos, geom);
		m_static_texts.emplace_back(utf8_to_wide(label), rect, e);
		return;
	}
	errorstream<< "Invalid ItemImagebutton element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseBox(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if ((parts.size() == 3) ||
		((parts.size() > 3) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		std::vector<std::string> v_pos = split(parts[0],',');
		std::vector<std::string> v_geom = split(parts[1],',');

		MY_CHECKPOS("box",0);
		MY_CHECKGEOM("box",1);

		v2s32 pos;
		v2s32 geom;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(true, v_pos);
			geom = getRealCoordinateGeometry(v_geom);
		} else {
			pos = getElementBasePos(true, &v_pos);
			geom.X = stof(v_geom[0]) * spacing.X;
			geom.Y = stof(v_geom[1]) * spacing.Y;
		}

		video::SColor tmp_color;

		if (parseColorString(parts[2], tmp_color, false, 0x8C)) {
			BoxDrawSpec spec(pos, geom, tmp_color);

			m_boxes.push_back(spec);
		}
		else {
			errorstream<< "Invalid Box element(" << parts.size() << "): '" << element << "'  INVALID COLOR"  << std::endl;
		}
		return;
	}
	errorstream<< "Invalid Box element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseBackgroundColor(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if (((parts.size() == 1) || (parts.size() == 2)) ||
			((parts.size() > 2) && (m_formspec_version > FORMSPEC_API_VERSION))) {
		parseColorString(parts[0], m_bgcolor, false);

		if (parts.size() == 2) {
			std::string fullscreen = parts[1];
			m_bgfullscreen = is_yes(fullscreen);
		}

		return;
	}

	errorstream << "Invalid bgcolor element(" << parts.size() << "): '" << element << "'"
			<< std::endl;
}

void GUIFormSpecMenu::parseListColors(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if (((parts.size() == 2) || (parts.size() == 3) || (parts.size() == 5)) ||
		((parts.size() > 5) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		parseColorString(parts[0], m_slotbg_n, false);
		parseColorString(parts[1], m_slotbg_h, false);

		if (parts.size() >= 3) {
			if (parseColorString(parts[2], m_slotbordercolor, false)) {
				m_slotborder = true;
			}
		}
		if (parts.size() == 5) {
			video::SColor tmp_color;

			if (parseColorString(parts[3], tmp_color, false))
				m_default_tooltip_bgcolor = tmp_color;
			if (parseColorString(parts[4], tmp_color, false))
				m_default_tooltip_color = tmp_color;
		}
		return;
	}
	errorstream<< "Invalid listcolors element(" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseTooltip(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');
	if (parts.size() < 2) {
		errorstream << "Invalid tooltip element(" << parts.size() << "): '"
				<< element << "'"  << std::endl;
		return;
	}

	// Get mode and check size
	bool rect_mode = parts[0].find(',') != std::string::npos;
	size_t base_size = rect_mode ? 3 : 2;
	if (parts.size() != base_size && parts.size() != base_size + 2) {
		errorstream << "Invalid tooltip element(" << parts.size() << "): '"
				<< element << "'"  << std::endl;
		return;
	}

	// Read colors
	video::SColor bgcolor = m_default_tooltip_bgcolor;
	video::SColor color   = m_default_tooltip_color;
	if (parts.size() == base_size + 2 &&
			(!parseColorString(parts[base_size], bgcolor, false) ||
				!parseColorString(parts[base_size + 1], color, false))) {
		errorstream << "Invalid color in tooltip element(" << parts.size()
				<< "): '" << element << "'"  << std::endl;
		return;
	}

	// Make tooltip spec
	std::string text = unescape_string(parts[rect_mode ? 2 : 1]);
	TooltipSpec spec(utf8_to_wide(text), bgcolor, color);

	// Add tooltip
	if (rect_mode) {
		std::vector<std::string> v_pos  = split(parts[0], ',');
		std::vector<std::string> v_geom = split(parts[1], ',');

		MY_CHECKPOS("tooltip", 0);
		MY_CHECKGEOM("tooltip", 1);

		v2s32 pos;
		v2s32 geom;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(true, v_pos);
			geom = getRealCoordinateGeometry(v_geom);
		} else {
			pos = getElementBasePos(true, &v_pos);
			geom.X = stof(v_geom[0]) * spacing.X;
			geom.Y = stof(v_geom[1]) * spacing.Y;
		}

		irr::core::rect<s32> rect(pos, pos + geom);
		m_tooltip_rects.emplace_back(rect, spec);
	} else {
		m_tooltips[parts[0]] = spec;
	}
}

bool GUIFormSpecMenu::parseVersionDirect(const std::string &data)
{
	//some prechecks
	if (data.empty())
		return false;

	std::vector<std::string> parts = split(data,'[');

	if (parts.size() < 2) {
		return false;
	}

	if (parts[0] != "formspec_version") {
		return false;
	}

	if (is_number(parts[1])) {
		m_formspec_version = mystoi(parts[1]);
		return true;
	}

	return false;
}

bool GUIFormSpecMenu::parseSizeDirect(parserData* data, const std::string &element)
{
	if (element.empty())
		return false;

	std::vector<std::string> parts = split(element,'[');

	if (parts.size() < 2)
		return false;

	std::string type = trim(parts[0]);
	std::string description = trim(parts[1]);

	if (type != "size" && type != "invsize")
		return false;

	if (type == "invsize")
		log_deprecated("Deprecated formspec element \"invsize\" is used");

	parseSize(data, description);

	return true;
}

bool GUIFormSpecMenu::parsePositionDirect(parserData *data, const std::string &element)
{
	if (element.empty())
		return false;

	std::vector<std::string> parts = split(element, '[');

	if (parts.size() != 2)
		return false;

	std::string type = trim(parts[0]);
	std::string description = trim(parts[1]);

	if (type != "position")
		return false;

	parsePosition(data, description);

	return true;
}

void GUIFormSpecMenu::parsePosition(parserData *data, const std::string &element)
{
	std::vector<std::string> parts = split(element, ',');

	if (parts.size() == 2) {
		data->offset.X = stof(parts[0]);
		data->offset.Y = stof(parts[1]);
		return;
	}

	errorstream << "Invalid position element (" << parts.size() << "): '" << element << "'" << std::endl;
}

bool GUIFormSpecMenu::parseAnchorDirect(parserData *data, const std::string &element)
{
	if (element.empty())
		return false;

	std::vector<std::string> parts = split(element, '[');

	if (parts.size() != 2)
		return false;

	std::string type = trim(parts[0]);
	std::string description = trim(parts[1]);

	if (type != "anchor")
		return false;

	parseAnchor(data, description);

	return true;
}

void GUIFormSpecMenu::parseAnchor(parserData *data, const std::string &element)
{
	std::vector<std::string> parts = split(element, ',');

	if (parts.size() == 2) {
		data->anchor.X = stof(parts[0]);
		data->anchor.Y = stof(parts[1]);
		return;
	}

	errorstream << "Invalid anchor element (" << parts.size() << "): '" << element
			<< "'" << std::endl;
}

bool GUIFormSpecMenu::parseStyle(parserData *data, const std::string &element, bool style_type)
{
	std::vector<std::string> parts = split(element, ';');

	if (parts.size() < 2) {
		errorstream << "Invalid style element (" << parts.size() << "): '" << element
					<< "'" << std::endl;
		return false;
	}

	std::string selector = trim(parts[0]);
	if (selector.empty()) {
		errorstream << "Invalid style element (Selector required): '" << element
					<< "'" << std::endl;
		return false;
	}

	StyleSpec spec;

	for (size_t i = 1; i < parts.size(); i++) {
		size_t equal_pos = parts[i].find('=');
		if (equal_pos == std::string::npos) {
			errorstream << "Invalid style element (Property missing value): '" << element
						<< "'" << std::endl;
			return false;
		}

		std::string propname = trim(parts[i].substr(0, equal_pos));
		std::string value    = trim(unescape_string(parts[i].substr(equal_pos + 1)));

		std::transform(propname.begin(), propname.end(), propname.begin(), ::tolower);

		StyleSpec::Property prop = StyleSpec::GetPropertyByName(propname);
		if (prop == StyleSpec::NONE) {
			if (property_warned.find(propname) != property_warned.end()) {
				warningstream << "Invalid style element (Unknown property " << propname << "): '"
						<< element
						<< "'" << std::endl;
				property_warned.insert(propname);
			}
			return false;
		}

		spec.set(prop, value);
	}

	if (style_type) {
		theme_by_type[selector] |= spec;
	} else {
		theme_by_name[selector] |= spec;
	}

	return true;
}

void GUIFormSpecMenu::parseElement(parserData* data, const std::string &element)
{
	//some prechecks
	if (element.empty())
		return;

	if (parseVersionDirect(element))
		return;

	std::vector<std::string> parts = split(element,'[');

	// ugly workaround to keep compatibility
	if (parts.size() > 2) {
		if (trim(parts[0]) == "image") {
			for (unsigned int i=2;i< parts.size(); i++) {
				parts[1] += "[" + parts[i];
			}
		}
		else { return; }
	}

	if (parts.size() < 2) {
		return;
	}

	std::string type = trim(parts[0]);
	std::string description = trim(parts[1]);

	if (type == "container") {
		parseContainer(data, description);
		return;
	}

	if (type == "container_end") {
		parseContainerEnd(data);
		return;
	}

	if (type == "list") {
		parseList(data, description);
		return;
	}

	if (type == "listring") {
		parseListRing(data, description);
		return;
	}

	if (type == "checkbox") {
		parseCheckbox(data, description);
		return;
	}

	if (type == "image") {
		parseImage(data, description);
		return;
	}

	if (type == "item_image") {
		parseItemImage(data, description);
		return;
	}

	if (type == "button" || type == "button_exit") {
		parseButton(data, description, type);
		return;
	}

	if (type == "background" || type == "background9") {
		parseBackground(data, description);
		return;
	}

	if (type == "tableoptions"){
		parseTableOptions(data,description);
		return;
	}

	if (type == "tablecolumns"){
		parseTableColumns(data,description);
		return;
	}

	if (type == "table"){
		parseTable(data,description);
		return;
	}

	if (type == "textlist"){
		parseTextList(data,description);
		return;
	}

	if (type == "dropdown"){
		parseDropDown(data,description);
		return;
	}

	if (type == "field_close_on_enter") {
		parseFieldCloseOnEnter(data, description);
		return;
	}

	if (type == "pwdfield") {
		parsePwdField(data,description);
		return;
	}

	if ((type == "field") || (type == "textarea")){
		parseField(data,description,type);
		return;
	}

	if (type == "hypertext") {
		parseHyperText(data,description);
		return;
	}

	if (type == "label") {
		parseLabel(data,description);
		return;
	}

	if (type == "vertlabel") {
		parseVertLabel(data,description);
		return;
	}

	if (type == "item_image_button") {
		parseItemImageButton(data,description);
		return;
	}

	if ((type == "image_button") || (type == "image_button_exit")) {
		parseImageButton(data,description,type);
		return;
	}

	if (type == "tabheader") {
		parseTabHeader(data,description);
		return;
	}

	if (type == "box") {
		parseBox(data,description);
		return;
	}

	if (type == "bgcolor") {
		parseBackgroundColor(data,description);
		return;
	}

	if (type == "listcolors") {
		parseListColors(data,description);
		return;
	}

	if (type == "tooltip") {
		parseTooltip(data,description);
		return;
	}

	if (type == "scrollbar") {
		parseScrollBar(data, description);
		return;
	}

	if (type == "real_coordinates") {
		data->real_coordinates = is_yes(description);
		return;
	}

	if (type == "style") {
		parseStyle(data, description, false);
		return;
	}

	if (type == "style_type") {
		parseStyle(data, description, true);
		return;
	}

	// Ignore others
	infostream << "Unknown DrawSpec: type=" << type << ", data=\"" << description << "\""
			<< std::endl;
}

void GUIFormSpecMenu::regenerateGui(v2u32 screensize)
{
	/* useless to regenerate without a screensize */
	if ((screensize.X <= 0) || (screensize.Y <= 0)) {
		return;
	}

	parserData mydata;

	//preserve tables
	for (auto &m_table : m_tables) {
		std::string tablename = m_table.first.fname;
		GUITable *table = m_table.second;
		mydata.table_dyndata[tablename] = table->getDynamicData();
	}

	//set focus
	if (!m_focused_element.empty())
		mydata.focused_fieldname = m_focused_element;

	//preserve focus
	gui::IGUIElement *focused_element = Environment->getFocus();
	if (focused_element && focused_element->getParent() == this) {
		s32 focused_id = focused_element->getID();
		if (focused_id > 257) {
			for (const GUIFormSpecMenu::FieldSpec &field : m_fields) {
				if (field.fid == focused_id) {
					mydata.focused_fieldname = field.fname;
					break;
				}
			}
		}
	}

	// Remove children
	removeChildren();

	for (auto &table_it : m_tables) {
		table_it.second->drop();
	}

	mydata.size= v2s32(100,100);
	mydata.screensize = screensize;
	mydata.offset = v2f32(0.5f, 0.5f);
	mydata.anchor = v2f32(0.5f, 0.5f);

	// Base position of contents of form
	mydata.basepos = getBasePos();

	/* Convert m_init_draw_spec to m_inventorylists */

	m_inventorylists.clear();
	m_images.clear();
	m_backgrounds.clear();
	m_itemimages.clear();
	m_tables.clear();
	m_checkboxes.clear();
	m_scrollbars.clear();
	m_fields.clear();
	m_boxes.clear();
	m_tooltips.clear();
	m_tooltip_rects.clear();
	m_inventory_rings.clear();
	m_static_texts.clear();
	m_dropdowns.clear();
	theme_by_name.clear();
	theme_by_type.clear();

	m_bgfullscreen = false;

	m_formspec_version = 1;

	{
		v3f formspec_bgcolor = g_settings->getV3F("formspec_default_bg_color");
		m_bgcolor = video::SColor(
			(u8) clamp_u8(g_settings->getS32("formspec_default_bg_opacity")),
			clamp_u8(myround(formspec_bgcolor.X)),
			clamp_u8(myround(formspec_bgcolor.Y)),
			clamp_u8(myround(formspec_bgcolor.Z))
		);
	}

	{
		v3f formspec_bgcolor = g_settings->getV3F("formspec_fullscreen_bg_color");
		m_fullscreen_bgcolor = video::SColor(
			(u8) clamp_u8(g_settings->getS32("formspec_fullscreen_bg_opacity")),
			clamp_u8(myround(formspec_bgcolor.X)),
			clamp_u8(myround(formspec_bgcolor.Y)),
			clamp_u8(myround(formspec_bgcolor.Z))
		);
	}

	m_slotbg_n = video::SColor(255,128,128,128);
	m_slotbg_h = video::SColor(255,192,192,192);

	m_default_tooltip_bgcolor = video::SColor(255,110,130,60);
	m_default_tooltip_color = video::SColor(255,255,255,255);

	m_slotbordercolor = video::SColor(200,0,0,0);
	m_slotborder = false;

	// Add tooltip
	{
		assert(!m_tooltip_element);
		// Note: parent != this so that the tooltip isn't clipped by the menu rectangle
		m_tooltip_element = gui::StaticText::add(Environment, L"",
			core::rect<s32>(0, 0, 110, 18));
		m_tooltip_element->enableOverrideColor(true);
		m_tooltip_element->setBackgroundColor(m_default_tooltip_bgcolor);
		m_tooltip_element->setDrawBackground(true);
		m_tooltip_element->setDrawBorder(true);
		m_tooltip_element->setOverrideColor(m_default_tooltip_color);
		m_tooltip_element->setTextAlignment(gui::EGUIA_CENTER, gui::EGUIA_CENTER);
		m_tooltip_element->setWordWrap(false);
		//we're not parent so no autograb for this one!
		m_tooltip_element->grab();
	}

	std::vector<std::string> elements = split(m_formspec_string,']');
	unsigned int i = 0;

	/* try to read version from first element only */
	if (!elements.empty()) {
		if (parseVersionDirect(elements[0])) {
			i++;
		}
	}

	/* we need size first in order to calculate image scale */
	mydata.explicit_size = false;
	for (; i< elements.size(); i++) {
		if (!parseSizeDirect(&mydata, elements[i])) {
			break;
		}
	}

	/* "position" element is always after "size" element if it used */
	for (; i< elements.size(); i++) {
		if (!parsePositionDirect(&mydata, elements[i])) {
			break;
		}
	}

	/* "anchor" element is always after "position" (or  "size" element) if it used */
	for (; i< elements.size(); i++) {
		if (!parseAnchorDirect(&mydata, elements[i])) {
			break;
		}
	}

	/* "no_prepend" element is always after "position" (or  "size" element) if it used */
	bool enable_prepends = true;
	for (; i < elements.size(); i++) {
		if (elements[i].empty())
			break;

		std::vector<std::string> parts = split(elements[i], '[');
		if (trim(parts[0]) == "no_prepend")
			enable_prepends = false;
		else
			break;
	}

	/* Copy of the "real_coordinates" element for after the form size. */
	mydata.real_coordinates = m_formspec_version >= 2;
	for (; i < elements.size(); i++) {
		std::vector<std::string> parts = split(elements[i], '[');
		std::string name = trim(parts[0]);
		if (name != "real_coordinates" || parts.size() != 2)
			break; // Invalid format

		mydata.real_coordinates = is_yes(trim(parts[1]));
	}

	if (mydata.explicit_size) {
		// compute scaling for specified form size
		if (m_lock) {
			v2u32 current_screensize = RenderingEngine::get_video_driver()->getScreenSize();
			v2u32 delta = current_screensize - m_lockscreensize;

			if (current_screensize.Y > m_lockscreensize.Y)
				delta.Y /= 2;
			else
				delta.Y = 0;

			if (current_screensize.X > m_lockscreensize.X)
				delta.X /= 2;
			else
				delta.X = 0;

			offset = v2s32(delta.X,delta.Y);

			mydata.screensize = m_lockscreensize;
		} else {
			offset = v2s32(0,0);
		}

		double gui_scaling = g_settings->getFloat("gui_scaling");
		double screen_dpi = RenderingEngine::getDisplayDensity() * 96;

		double use_imgsize;
		if (m_lock) {
			// In fixed-size mode, inventory image size
			// is 0.53 inch multiplied by the gui_scaling
			// config parameter.  This magic size is chosen
			// to make the main menu (15.5 inventory images
			// wide, including border) just fit into the
			// default window (800 pixels wide) at 96 DPI
			// and default scaling (1.00).
			use_imgsize = 0.5555 * screen_dpi * gui_scaling;
		} else {
			// In variable-size mode, we prefer to make the
			// inventory image size 1/15 of screen height,
			// multiplied by the gui_scaling config parameter.
			// If the preferred size won't fit the whole
			// form on the screen, either horizontally or
			// vertically, then we scale it down to fit.
			// (The magic numbers in the computation of what
			// fits arise from the scaling factors in the
			// following stanza, including the form border,
			// help text space, and 0.1 inventory slot spare.)
			// However, a minimum size is also set, that
			// the image size can't be less than 0.3 inch
			// multiplied by gui_scaling, even if this means
			// the form doesn't fit the screen.
#ifdef __ANDROID__
			// For mobile devices these magic numbers are
			// different and forms should always use the
			// maximum screen space available.
			double prefer_imgsize = mydata.screensize.Y / 10 * gui_scaling;
			double fitx_imgsize = mydata.screensize.X /
				((12.0 / 8.0) * (0.5 + mydata.invsize.X));
			double fity_imgsize = mydata.screensize.Y /
				((15.0 / 11.0) * (0.85 + mydata.invsize.Y));
			use_imgsize = MYMIN(prefer_imgsize,
					MYMIN(fitx_imgsize, fity_imgsize));
#else
			double prefer_imgsize = mydata.screensize.Y / 15 * gui_scaling;
			double fitx_imgsize = mydata.screensize.X /
				((5.0 / 4.0) * (0.5 + mydata.invsize.X));
			double fity_imgsize = mydata.screensize.Y /
				((15.0 / 13.0) * (0.85 * mydata.invsize.Y));
			double screen_dpi = RenderingEngine::getDisplayDensity() * 96;
			double min_imgsize = 0.3 * screen_dpi * gui_scaling;
			use_imgsize = MYMAX(min_imgsize, MYMIN(prefer_imgsize,
				MYMIN(fitx_imgsize, fity_imgsize)));
#endif
		}

		// Everything else is scaled in proportion to the
		// inventory image size.  The inventory slot spacing
		// is 5/4 image size horizontally and 15/13 image size
		// vertically.	The padding around the form (incorporating
		// the border of the outer inventory slots) is 3/8
		// image size.	Font height (baseline to baseline)
		// is 2/5 vertical inventory slot spacing, and button
		// half-height is 7/8 of font height.
		imgsize = v2s32(use_imgsize, use_imgsize);
		spacing = v2f32(use_imgsize*5.0/4, use_imgsize*15.0/13);
		padding = v2s32(use_imgsize*3.0/8, use_imgsize*3.0/8);
		m_btn_height = use_imgsize*15.0/13 * 0.35;

		m_font = g_fontengine->getFont();

		if (mydata.real_coordinates) {
			mydata.size = v2s32(
				mydata.invsize.X*imgsize.X,
				mydata.invsize.Y*imgsize.Y
			);
		} else {
			mydata.size = v2s32(
				padding.X*2+spacing.X*(mydata.invsize.X-1.0)+imgsize.X,
				padding.Y*2+spacing.Y*(mydata.invsize.Y-1.0)+imgsize.Y + m_btn_height*2.0/3.0
			);
		}

		DesiredRect = mydata.rect = core::rect<s32>(
				(s32)((f32)mydata.screensize.X * mydata.offset.X) - (s32)(mydata.anchor.X * (f32)mydata.size.X) + offset.X,
				(s32)((f32)mydata.screensize.Y * mydata.offset.Y) - (s32)(mydata.anchor.Y * (f32)mydata.size.Y) + offset.Y,
				(s32)((f32)mydata.screensize.X * mydata.offset.X) + (s32)((1.0 - mydata.anchor.X) * (f32)mydata.size.X) + offset.X,
				(s32)((f32)mydata.screensize.Y * mydata.offset.Y) + (s32)((1.0 - mydata.anchor.Y) * (f32)mydata.size.Y) + offset.Y
		);
	} else {
		// Non-size[] form must consist only of text fields and
		// implicit "Proceed" button.  Use default font, and
		// temporary form size which will be recalculated below.
		m_font = g_fontengine->getFont();
		m_btn_height = font_line_height(m_font) * 0.875;
		DesiredRect = core::rect<s32>(
			(s32)((f32)mydata.screensize.X * mydata.offset.X) - (s32)(mydata.anchor.X * 580.0),
			(s32)((f32)mydata.screensize.Y * mydata.offset.Y) - (s32)(mydata.anchor.Y * 300.0),
			(s32)((f32)mydata.screensize.X * mydata.offset.X) + (s32)((1.0 - mydata.anchor.X) * 580.0),
			(s32)((f32)mydata.screensize.Y * mydata.offset.Y) + (s32)((1.0 - mydata.anchor.Y) * 300.0)
		);
	}
	recalculateAbsolutePosition(false);
	mydata.basepos = getBasePos();
	m_tooltip_element->setOverrideFont(m_font);

	gui::IGUISkin *skin = Environment->getSkin();
	sanity_check(skin);
	gui::IGUIFont *old_font = skin->getFont();
	skin->setFont(m_font);

	pos_offset = v2f32();

	if (enable_prepends) {
		// Backup the coordinates so that prepends can use the coordinates of choice.
		bool rc_backup = mydata.real_coordinates;
		u16 version_backup = m_formspec_version;
		mydata.real_coordinates = false; // Old coordinates by default.

		std::vector<std::string> prepend_elements = split(m_formspec_prepend, ']');
		for (const auto &element : prepend_elements)
			parseElement(&mydata, element);

		m_formspec_version = version_backup;
		mydata.real_coordinates = rc_backup; // Restore coordinates
	}

	for (; i< elements.size(); i++) {
		parseElement(&mydata, elements[i]);
	}

	if (!container_stack.empty()) {
		errorstream << "Invalid formspec string: container was never closed!"
			<< std::endl;
	}

	// If there are fields without explicit size[], add a "Proceed"
	// button and adjust size to fit all the fields.
	if (!m_fields.empty() && !mydata.explicit_size) {
		mydata.rect = core::rect<s32>(
				mydata.screensize.X/2 - 580/2,
				mydata.screensize.Y/2 - 300/2,
				mydata.screensize.X/2 + 580/2,
				mydata.screensize.Y/2 + 240/2+(m_fields.size()*60)
		);
		DesiredRect = mydata.rect;
		recalculateAbsolutePosition(false);
		mydata.basepos = getBasePos();

		{
			v2s32 pos = mydata.basepos;
			pos.Y = ((m_fields.size()+2)*60);

			v2s32 size = DesiredRect.getSize();
			mydata.rect =
					core::rect<s32>(size.X/2-70, pos.Y,
							(size.X/2-70)+140, pos.Y + (m_btn_height*2));
			const wchar_t *text = wgettext("Proceed");
			GUIButton::addButton(Environment, mydata.rect, this, 257, text);
			delete[] text;
		}

	}

	//set initial focus if parser didn't set it
	focused_element = Environment->getFocus();
	if (!focused_element
			|| !isMyChild(focused_element)
			|| focused_element->getType() == gui::EGUIET_TAB_CONTROL)
		setInitialFocus();

	skin->setFont(old_font);
}

#ifdef __ANDROID__
bool GUIFormSpecMenu::getAndroidUIInput()
{
	if (!hasAndroidUIInput())
		return false;

	std::string fieldname = m_jni_field_name;
	m_jni_field_name.clear();

	for(std::vector<FieldSpec>::iterator iter =  m_fields.begin();
			iter != m_fields.end(); ++iter) {

		if (iter->fname != fieldname) {
			continue;
		}
		IGUIElement* tochange = getElementFromId(iter->fid);

		if (tochange == 0) {
			return false;
		}

		if (tochange->getType() != irr::gui::EGUIET_EDIT_BOX) {
			return false;
		}

		std::string text = porting::getInputDialogValue();

		((gui::IGUIEditBox *)tochange)->setText(utf8_to_wide(text).c_str());
	}
	return false;
}
#endif

GUIFormSpecMenu::ItemSpec GUIFormSpecMenu::getItemAtPos(v2s32 p) const
{
	core::rect<s32> imgrect(0,0,imgsize.X,imgsize.Y);

	for (const GUIFormSpecMenu::ListDrawSpec &s : m_inventorylists) {
		for(s32 i=0; i<s.geom.X*s.geom.Y; i++) {
			s32 item_i = i + s.start_item_i;

			s32 x;
			s32 y;
			if (s.real_coordinates) {
				x = (i%s.geom.X) * (imgsize.X * 1.25);
				y = (i/s.geom.X) * (imgsize.Y * 1.25);
			} else {
				x = (i%s.geom.X) * spacing.X;
				y = (i/s.geom.X) * spacing.Y;
			}
			v2s32 p0(x,y);
			core::rect<s32> rect = imgrect + s.pos + p0;
			if(rect.isPointInside(p))
			{
				return ItemSpec(s.inventoryloc, s.listname, item_i);
			}
		}
	}

	return ItemSpec(InventoryLocation(), "", -1);
}

void GUIFormSpecMenu::drawList(const ListDrawSpec &s, int layer,
		bool &item_hovered)
{
	video::IVideoDriver* driver = Environment->getVideoDriver();

	Inventory *inv = m_invmgr->getInventory(s.inventoryloc);
	if(!inv){
		warningstream<<"GUIFormSpecMenu::drawList(): "
				<<"The inventory location "
				<<"\""<<s.inventoryloc.dump()<<"\" doesn't exist"
				<<std::endl;
		return;
	}
	InventoryList *ilist = inv->getList(s.listname);
	if(!ilist){
		warningstream<<"GUIFormSpecMenu::drawList(): "
				<<"The inventory list \""<<s.listname<<"\" @ \""
				<<s.inventoryloc.dump()<<"\" doesn't exist"
				<<std::endl;
		return;
	}

	core::rect<s32> imgrect(0,0,imgsize.X,imgsize.Y);

	for (s32 i = 0; i < s.geom.X * s.geom.Y; i++) {
		s32 item_i = i + s.start_item_i;
		if (item_i >= (s32)ilist->getSize())
			break;

		s32 x;
		s32 y;
		if (s.real_coordinates) {
			x = (i%s.geom.X) * (imgsize.X * 1.25);
			y = (i/s.geom.X) * (imgsize.Y * 1.25);
		} else {
			x = (i%s.geom.X) * spacing.X;
			y = (i/s.geom.X) * spacing.Y;
		}
		v2s32 p(x,y);
		core::rect<s32> rect = imgrect + s.pos + p;
		ItemStack item = ilist->getItem(item_i);

		bool selected = m_selected_item
			&& m_invmgr->getInventory(m_selected_item->inventoryloc) == inv
			&& m_selected_item->listname == s.listname
			&& m_selected_item->i == item_i;
		bool hovering = rect.isPointInside(m_pointer);
		ItemRotationKind rotation_kind = selected ? IT_ROT_SELECTED :
			(hovering ? IT_ROT_HOVERED : IT_ROT_NONE);

		if (layer == 0) {
			if (hovering) {
				item_hovered = true;
				driver->draw2DRectangle(m_slotbg_h, rect, &AbsoluteClippingRect);
			} else {
				driver->draw2DRectangle(m_slotbg_n, rect, &AbsoluteClippingRect);
			}
		}

		//Draw inv slot borders
		if (m_slotborder) {
			s32 x1 = rect.UpperLeftCorner.X;
			s32 y1 = rect.UpperLeftCorner.Y;
			s32 x2 = rect.LowerRightCorner.X;
			s32 y2 = rect.LowerRightCorner.Y;
			s32 border = 1;
			driver->draw2DRectangle(m_slotbordercolor,
				core::rect<s32>(v2s32(x1 - border, y1 - border),
								v2s32(x2 + border, y1)), NULL);
			driver->draw2DRectangle(m_slotbordercolor,
				core::rect<s32>(v2s32(x1 - border, y2),
								v2s32(x2 + border, y2 + border)), NULL);
			driver->draw2DRectangle(m_slotbordercolor,
				core::rect<s32>(v2s32(x1 - border, y1),
								v2s32(x1, y2)), NULL);
			driver->draw2DRectangle(m_slotbordercolor,
				core::rect<s32>(v2s32(x2, y1),
								v2s32(x2 + border, y2)), NULL);
		}

		if (layer == 1) {
			if (selected)
				item.takeItem(m_selected_amount);

			if (!item.empty()) {
				// Draw item stack
				drawItemStack(driver, m_font, item,
					rect, &AbsoluteClippingRect, m_client, rotation_kind);

				// Draw tooltip
				if (hovering && !m_selected_item) {
					std::string tooltip = item.getDescription(m_client->idef());
					if (m_tooltip_append_itemname)
						tooltip += "\n[" + item.name + "]";
					showTooltip(utf8_to_wide(tooltip), m_default_tooltip_color,
							m_default_tooltip_bgcolor);
				}
			}
		}
	}
}

void GUIFormSpecMenu::drawSelectedItem()
{
	video::IVideoDriver* driver = Environment->getVideoDriver();

	if (!m_selected_item) {
		drawItemStack(driver, m_font, ItemStack(),
				core::rect<s32>(v2s32(0, 0), v2s32(0, 0)), NULL,
				m_client, IT_ROT_DRAGGED);
		return;
	}

	Inventory *inv = m_invmgr->getInventory(m_selected_item->inventoryloc);
	sanity_check(inv);
	InventoryList *list = inv->getList(m_selected_item->listname);
	sanity_check(list);
	ItemStack stack = list->getItem(m_selected_item->i);
	stack.count = m_selected_amount;

	core::rect<s32> imgrect(0,0,imgsize.X,imgsize.Y);
	core::rect<s32> rect = imgrect + (m_pointer - imgrect.getCenter());
	rect.constrainTo(driver->getViewPort());
	drawItemStack(driver, m_font, stack, rect, NULL, m_client, IT_ROT_DRAGGED);
}

void GUIFormSpecMenu::drawMenu()
{
	if (m_form_src) {
		const std::string &newform = m_form_src->getForm();
		if (newform != m_formspec_string) {
			m_formspec_string = newform;
			regenerateGui(m_screensize_old);
		}
	}

	gui::IGUISkin* skin = Environment->getSkin();
	sanity_check(skin != NULL);
	gui::IGUIFont *old_font = skin->getFont();
	skin->setFont(m_font);

	updateSelectedItem();

	video::IVideoDriver* driver = Environment->getVideoDriver();

	v2u32 screenSize = driver->getScreenSize();
	core::rect<s32> allbg(0, 0, screenSize.X, screenSize.Y);

	if (m_bgfullscreen)
		driver->draw2DRectangle(m_fullscreen_bgcolor, allbg, &allbg);
	else
		driver->draw2DRectangle(m_bgcolor, AbsoluteRect, &AbsoluteClippingRect);

	m_tooltip_element->setVisible(false);

	for (const auto &pair : m_tooltip_rects) {
		if (pair.first.isPointInside(m_pointer)) {
			const std::wstring &text = pair.second.tooltip;
			if (!text.empty()) {
				showTooltip(text, pair.second.color, pair.second.bgcolor);
				break;
			}
		}
	}

	/*
		Draw backgrounds
	*/
	for (const GUIFormSpecMenu::ImageDrawSpec &spec : m_backgrounds) {
		video::ITexture *texture = m_tsrc->getTexture(spec.name);

		if (texture != 0) {
			// Image size on screen
			core::rect<s32> imgrect(0, 0, spec.geom.X, spec.geom.Y);
			// Image rectangle on screen
			core::rect<s32> rect = imgrect + spec.pos;
			// Middle rect for 9-slicing
			core::rect<s32> middle = spec.middle;

			if (spec.clip) {
				core::dimension2d<s32> absrec_size = AbsoluteRect.getSize();
				rect = core::rect<s32>(AbsoluteRect.UpperLeftCorner.X - spec.pos.X,
									AbsoluteRect.UpperLeftCorner.Y - spec.pos.Y,
									AbsoluteRect.UpperLeftCorner.X + absrec_size.Width + spec.pos.X,
									AbsoluteRect.UpperLeftCorner.Y + absrec_size.Height + spec.pos.Y);
			}

			if (middle.getArea() == 0) {
				const video::SColor color(255, 255, 255, 255);
				const video::SColor colors[] = {color, color, color, color};
				draw2DImageFilterScaled(driver, texture, rect,
						core::rect<s32>(core::position2d<s32>(0, 0),
								core::dimension2di(texture->getOriginalSize())),
						NULL/*&AbsoluteClippingRect*/, colors, true);
			} else {
				// `-x` is interpreted as `w - x`
				if (middle.LowerRightCorner.X < 0) {
					middle.LowerRightCorner.X += texture->getOriginalSize().Width;
				}
				if (middle.LowerRightCorner.Y < 0) {
					middle.LowerRightCorner.Y += texture->getOriginalSize().Height;
				}
				draw2DImage9Slice(driver, texture, rect, middle);
			}
		} else {
			errorstream << "GUIFormSpecMenu::drawMenu() Draw backgrounds unable to load texture:" << std::endl;
			errorstream << "\t" << spec.name << std::endl;
		}
	}

	/*
		Draw Boxes
	*/
	for (const GUIFormSpecMenu::BoxDrawSpec &spec : m_boxes) {
		irr::video::SColor todraw = spec.color;

		core::rect<s32> rect(spec.pos.X,spec.pos.Y,
							spec.pos.X + spec.geom.X,spec.pos.Y + spec.geom.Y);

		driver->draw2DRectangle(todraw, rect, 0);
	}

	/*
		Call base class
	*/
	gui::IGUIElement::draw();

	/*
		Draw images
	*/
	for (const GUIFormSpecMenu::ImageDrawSpec &spec : m_images) {
		video::ITexture *texture = m_tsrc->getTexture(spec.name);

		if (texture != 0) {
			const core::dimension2d<u32>& img_origsize = texture->getOriginalSize();
			// Image size on screen
			core::rect<s32> imgrect;

			if (spec.scale)
				imgrect = core::rect<s32>(0,0,spec.geom.X, spec.geom.Y);
			else {

				imgrect = core::rect<s32>(0,0,img_origsize.Width,img_origsize.Height);
			}
			// Image rectangle on screen
			core::rect<s32> rect = imgrect + spec.pos;
			const video::SColor color(255,255,255,255);
			const video::SColor colors[] = {color,color,color,color};
			draw2DImageFilterScaled(driver, texture, rect,
				core::rect<s32>(core::position2d<s32>(0,0),img_origsize),
				NULL/*&AbsoluteClippingRect*/, colors, true);
		}
		else {
			errorstream << "GUIFormSpecMenu::drawMenu() Draw images unable to load texture:" << std::endl;
			errorstream << "\t" << spec.name << std::endl;
		}
	}

	/*
		Draw item images
	*/
	for (const GUIFormSpecMenu::ImageDrawSpec &spec : m_itemimages) {
		if (m_client == 0)
			break;

		IItemDefManager *idef = m_client->idef();
		ItemStack item;
		item.deSerialize(spec.item_name, idef);
		core::rect<s32> imgrect(0, 0, spec.geom.X, spec.geom.Y);
		// Viewport rectangle on screen
		core::rect<s32> rect = imgrect + spec.pos;
		if (spec.parent_button && spec.parent_button->isPressed()) {
#if (IRRLICHT_VERSION_MAJOR == 1 && IRRLICHT_VERSION_MINOR < 8)
			rect += core::dimension2d<s32>(
				0.05 * (float)rect.getWidth(), 0.05 * (float)rect.getHeight());
#else
			rect += core::dimension2d<s32>(
				skin->getSize(irr::gui::EGDS_BUTTON_PRESSED_IMAGE_OFFSET_X),
				skin->getSize(irr::gui::EGDS_BUTTON_PRESSED_IMAGE_OFFSET_Y));
#endif
		}
		drawItemStack(driver, m_font, item, rect, &AbsoluteClippingRect,
				m_client, IT_ROT_NONE);
	}

	/*
		Draw items
		Layer 0: Item slot rectangles
		Layer 1: Item images; prepare tooltip
	*/
	bool item_hovered = false;
	for (int layer = 0; layer < 2; layer++) {
		for (const GUIFormSpecMenu::ListDrawSpec &spec : m_inventorylists) {
			drawList(spec, layer, item_hovered);
		}
	}
	if (!item_hovered) {
		drawItemStack(driver, m_font, ItemStack(),
			core::rect<s32>(v2s32(0, 0), v2s32(0, 0)),
			NULL, m_client, IT_ROT_HOVERED);
	}

/* TODO find way to show tooltips on touchscreen */
#ifndef HAVE_TOUCHSCREENGUI
	m_pointer = RenderingEngine::get_raw_device()->getCursorControl()->getPosition();
#endif

	/*
		Draw static text elements
	*/
	for (const GUIFormSpecMenu::StaticTextSpec &spec : m_static_texts) {
		core::rect<s32> rect = spec.rect;
		if (spec.parent_button && spec.parent_button->isPressed()) {
#if (IRRLICHT_VERSION_MAJOR == 1 && IRRLICHT_VERSION_MINOR < 8)
			rect += core::dimension2d<s32>(
				0.05 * (float)rect.getWidth(), 0.05 * (float)rect.getHeight());
#else
			// Use image offset instead of text's because its a bit smaller
			// and fits better, also TEXT_OFFSET_X is always 0
			rect += core::dimension2d<s32>(
				skin->getSize(irr::gui::EGDS_BUTTON_PRESSED_IMAGE_OFFSET_X),
				skin->getSize(irr::gui::EGDS_BUTTON_PRESSED_IMAGE_OFFSET_Y));
#endif
		}
		video::SColor color(255, 255, 255, 255);
		m_font->draw(spec.text.c_str(), rect, color, true, true, &rect);
	}

	/*
		Draw fields/buttons tooltips
	*/
	gui::IGUIElement *hovered =
			Environment->getRootGUIElement()->getElementFromPoint(m_pointer);

	if (hovered != NULL) {
		s32 id = hovered->getID();

		u64 delta = 0;
		if (id == -1) {
			m_old_tooltip_id = id;
		} else {
			if (id == m_old_tooltip_id) {
				delta = porting::getDeltaMs(m_hovered_time, porting::getTimeMs());
			} else {
				m_hovered_time = porting::getTimeMs();
				m_old_tooltip_id = id;
			}
		}

		// Find and update the current tooltip
		if (id != -1 && delta >= m_tooltip_show_delay) {
			for (const FieldSpec &field : m_fields) {

				if (field.fid != id)
					continue;

				const std::wstring &text = m_tooltips[field.fname].tooltip;
				if (!text.empty())
					showTooltip(text, m_tooltips[field.fname].color,
						m_tooltips[field.fname].bgcolor);

				break;
			}
		}
	}

	m_tooltip_element->draw();

	/*
		Draw dragged item stack
	*/
	drawSelectedItem();

	skin->setFont(old_font);
}


void GUIFormSpecMenu::showTooltip(const std::wstring &text,
	const irr::video::SColor &color, const irr::video::SColor &bgcolor)
{
	const std::wstring ntext = translate_string(text);
	m_tooltip_element->setOverrideColor(color);
	m_tooltip_element->setBackgroundColor(bgcolor);
	setStaticText(m_tooltip_element, ntext.c_str());

	// Tooltip size and offset
	s32 tooltip_width = m_tooltip_element->getTextWidth() + m_btn_height;
#if (IRRLICHT_VERSION_MAJOR <= 1 && IRRLICHT_VERSION_MINOR <= 8 && IRRLICHT_VERSION_REVISION < 2) || USE_FREETYPE == 1
	std::vector<std::wstring> text_rows = str_split(ntext, L'\n');
	s32 tooltip_height = m_tooltip_element->getTextHeight() * text_rows.size() + 5;
#else
	s32 tooltip_height = m_tooltip_element->getTextHeight() + 5;
#endif
	v2u32 screenSize = Environment->getVideoDriver()->getScreenSize();
	int tooltip_offset_x = m_btn_height;
	int tooltip_offset_y = m_btn_height;
#ifdef __ANDROID__
	tooltip_offset_x *= 3;
	tooltip_offset_y  = 0;
	if (m_pointer.X > (s32)screenSize.X / 2)
		tooltip_offset_x = -(tooltip_offset_x + tooltip_width);
#endif

	// Calculate and set the tooltip position
	s32 tooltip_x = m_pointer.X + tooltip_offset_x;
	s32 tooltip_y = m_pointer.Y + tooltip_offset_y;
	if (tooltip_x + tooltip_width > (s32)screenSize.X)
		tooltip_x = (s32)screenSize.X - tooltip_width  - m_btn_height;
	if (tooltip_y + tooltip_height > (s32)screenSize.Y)
		tooltip_y = (s32)screenSize.Y - tooltip_height - m_btn_height;

	m_tooltip_element->setRelativePosition(
		core::rect<s32>(
			core::position2d<s32>(tooltip_x, tooltip_y),
			core::dimension2d<s32>(tooltip_width, tooltip_height)
		)
	);

	// Display the tooltip
	m_tooltip_element->setVisible(true);
	bringToFront(m_tooltip_element);
}

void GUIFormSpecMenu::updateSelectedItem()
{
	verifySelectedItem();

	// If craftresult is nonempty and nothing else is selected, select it now.
	if (!m_selected_item) {
		for (const GUIFormSpecMenu::ListDrawSpec &s : m_inventorylists) {
			if (s.listname != "craftpreview")
				continue;

			Inventory *inv = m_invmgr->getInventory(s.inventoryloc);
			if (!inv)
				continue;

			InventoryList *list = inv->getList("craftresult");

			if (!list || list->getSize() == 0)
				continue;

			const ItemStack &item = list->getItem(0);
			if (item.empty())
				continue;

			// Grab selected item from the crafting result list
			m_selected_item = new ItemSpec;
			m_selected_item->inventoryloc = s.inventoryloc;
			m_selected_item->listname = "craftresult";
			m_selected_item->i = 0;
			m_selected_amount = item.count;
			m_selected_dragging = false;
			break;
		}
	}

	// If craftresult is selected, keep the whole stack selected
	if (m_selected_item && m_selected_item->listname == "craftresult")
		m_selected_amount = verifySelectedItem().count;
}

ItemStack GUIFormSpecMenu::verifySelectedItem()
{
	// If the selected stack has become empty for some reason, deselect it.
	// If the selected stack has become inaccessible, deselect it.
	// If the selected stack has become smaller, adjust m_selected_amount.
	// Return the selected stack.

	if(m_selected_item)
	{
		if(m_selected_item->isValid())
		{
			Inventory *inv = m_invmgr->getInventory(m_selected_item->inventoryloc);
			if(inv)
			{
				InventoryList *list = inv->getList(m_selected_item->listname);
				if(list && (u32) m_selected_item->i < list->getSize())
				{
					ItemStack stack = list->getItem(m_selected_item->i);
					if (!m_selected_swap.empty()) {
						if (m_selected_swap.name == stack.name &&
								m_selected_swap.count == stack.count)
							m_selected_swap.clear();
					} else {
						m_selected_amount = std::min(m_selected_amount, stack.count);
					}

					if (!stack.empty())
						return stack;
				}
			}
		}

		// selection was not valid
		delete m_selected_item;
		m_selected_item = NULL;
		m_selected_amount = 0;
		m_selected_dragging = false;
	}
	return ItemStack();
}

void GUIFormSpecMenu::acceptInput(FormspecQuitMode quitmode=quit_mode_no)
{
	if(m_text_dst)
	{
		StringMap fields;

		if (quitmode == quit_mode_accept) {
			fields["quit"] = "true";
		}

		if (quitmode == quit_mode_cancel) {
			fields["quit"] = "true";
			m_text_dst->gotText(fields);
			return;
		}

		if (current_keys_pending.key_down) {
			fields["key_down"] = "true";
			current_keys_pending.key_down = false;
		}

		if (current_keys_pending.key_up) {
			fields["key_up"] = "true";
			current_keys_pending.key_up = false;
		}

		if (current_keys_pending.key_enter) {
			fields["key_enter"] = "true";
			current_keys_pending.key_enter = false;
		}

		if (!current_field_enter_pending.empty()) {
			fields["key_enter_field"] = current_field_enter_pending;
			current_field_enter_pending = "";
		}

		if (current_keys_pending.key_escape) {
			fields["key_escape"] = "true";
			current_keys_pending.key_escape = false;
		}

		for (const GUIFormSpecMenu::FieldSpec &s : m_fields) {
			if(s.send) {
				std::string name = s.fname;
				if (s.ftype == f_Button) {
					fields[name] = wide_to_utf8(s.flabel);
				} else if (s.ftype == f_Table) {
					GUITable *table = getTable(s.fname);
					if (table) {
						fields[name] = table->checkEvent();
					}
				}
				else if(s.ftype == f_DropDown) {
					// no dynamic cast possible due to some distributions shipped
					// without rtti support in irrlicht
					IGUIElement * element = getElementFromId(s.fid);
					gui::IGUIComboBox *e = NULL;
					if ((element) && (element->getType() == gui::EGUIET_COMBO_BOX)) {
						e = static_cast<gui::IGUIComboBox*>(element);
					}
					s32 selected = e->getSelected();
					if (selected >= 0) {
						std::vector<std::string> *dropdown_values =
							getDropDownValues(s.fname);
						if (dropdown_values && selected < (s32)dropdown_values->size()) {
							fields[name] = (*dropdown_values)[selected];
						}
					}
				}
				else if (s.ftype == f_TabHeader) {
					// no dynamic cast possible due to some distributions shipped
					// without rttzi support in irrlicht
					IGUIElement * element = getElementFromId(s.fid);
					gui::IGUITabControl *e = NULL;
					if ((element) && (element->getType() == gui::EGUIET_TAB_CONTROL)) {
						e = static_cast<gui::IGUITabControl *>(element);
					}

					if (e != 0) {
						std::stringstream ss;
						ss << (e->getActiveTab() +1);
						fields[name] = ss.str();
					}
				}
				else if (s.ftype == f_CheckBox) {
					// no dynamic cast possible due to some distributions shipped
					// without rtti support in irrlicht
					IGUIElement * element = getElementFromId(s.fid);
					gui::IGUICheckBox *e = NULL;
					if ((element) && (element->getType() == gui::EGUIET_CHECK_BOX)) {
						e = static_cast<gui::IGUICheckBox*>(element);
					}

					if (e != 0) {
						if (e->isChecked())
							fields[name] = "true";
						else
							fields[name] = "false";
					}
				}
				else if (s.ftype == f_ScrollBar) {
					// no dynamic cast possible due to some distributions shipped
					// without rtti support in irrlicht
					IGUIElement * element = getElementFromId(s.fid);
					gui::IGUIScrollBar *e = NULL;
					if ((element) && (element->getType() == gui::EGUIET_SCROLL_BAR)) {
						e = static_cast<gui::IGUIScrollBar*>(element);
					}

					if (e != 0) {
						std::stringstream os;
						os << e->getPos();
						if (s.fdefault == L"Changed")
							fields[name] = "CHG:" + os.str();
						else
							fields[name] = "VAL:" + os.str();
 					}
				}
				else
				{
					IGUIElement* e = getElementFromId(s.fid);
					if(e != NULL) {
						fields[name] = wide_to_utf8(e->getText());
					}
				}
			}
		}

		m_text_dst->gotText(fields);
	}
}

static bool isChild(gui::IGUIElement * tocheck, gui::IGUIElement * parent)
{
	while(tocheck != NULL) {
		if (tocheck == parent) {
			return true;
		}
		tocheck = tocheck->getParent();
	}
	return false;
}

bool GUIFormSpecMenu::preprocessEvent(const SEvent& event)
{
	// The IGUITabControl renders visually using the skin's selected
	// font, which we override for the duration of form drawing,
	// but computes tab hotspots based on how it would have rendered
	// using the font that is selected at the time of button release.
	// To make these two consistent, temporarily override the skin's
	// font while the IGUITabControl is processing the event.
	if (event.EventType == EET_MOUSE_INPUT_EVENT &&
			event.MouseInput.Event == EMIE_LMOUSE_LEFT_UP) {
		s32 x = event.MouseInput.X;
		s32 y = event.MouseInput.Y;
		gui::IGUIElement *hovered =
			Environment->getRootGUIElement()->getElementFromPoint(
				core::position2d<s32>(x, y));
		if (hovered && isMyChild(hovered) &&
				hovered->getType() == gui::EGUIET_TAB_CONTROL) {
			gui::IGUISkin* skin = Environment->getSkin();
			sanity_check(skin != NULL);
			gui::IGUIFont *old_font = skin->getFont();
			skin->setFont(m_font);
			bool retval = hovered->OnEvent(event);
			skin->setFont(old_font);
			return retval;
		}
	}

	// Fix Esc/Return key being eaten by checkboxen and tables
	if(event.EventType==EET_KEY_INPUT_EVENT) {
		KeyPress kp(event.KeyInput);
		if (kp == EscapeKey || kp == CancelKey
				|| kp == getKeySetting("keymap_inventory")
				|| event.KeyInput.Key==KEY_RETURN) {
			gui::IGUIElement *focused = Environment->getFocus();
			if (focused && isMyChild(focused) &&
					(focused->getType() == gui::EGUIET_LIST_BOX ||
					focused->getType() == gui::EGUIET_CHECK_BOX) &&
					(focused->getParent()->getType() != gui::EGUIET_COMBO_BOX ||
					event.KeyInput.Key != KEY_RETURN)) {
				OnEvent(event);
				return true;
			}
		}
	}
	// Mouse wheel and move events: send to hovered element instead of focused
	if (event.EventType == EET_MOUSE_INPUT_EVENT &&
			(event.MouseInput.Event == EMIE_MOUSE_WHEEL ||
			 event.MouseInput.Event == EMIE_MOUSE_MOVED)) {
		s32 x = event.MouseInput.X;
		s32 y = event.MouseInput.Y;
		gui::IGUIElement *hovered =
			Environment->getRootGUIElement()->getElementFromPoint(
				core::position2d<s32>(x, y));
		if (hovered && isMyChild(hovered)) {
			hovered->OnEvent(event);
			return event.MouseInput.Event == EMIE_MOUSE_WHEEL;
		}
	}

	if (event.EventType == EET_MOUSE_INPUT_EVENT) {
		s32 x = event.MouseInput.X;
		s32 y = event.MouseInput.Y;
		gui::IGUIElement *hovered =
			Environment->getRootGUIElement()->getElementFromPoint(
				core::position2d<s32>(x, y));
		if (event.MouseInput.Event == EMIE_LMOUSE_PRESSED_DOWN) {
			m_old_tooltip_id = -1;
		}
		if (!isChild(hovered,this)) {
			if (DoubleClickDetection(event)) {
				return true;
			}
		}
	}

	if (event.EventType == irr::EET_JOYSTICK_INPUT_EVENT) {
		/* TODO add a check like:
		if (event.JoystickEvent != joystick_we_listen_for)
			return false;
		*/
		bool handled = m_joystick->handleEvent(event.JoystickEvent);
		if (handled) {
			if (m_joystick->wasKeyDown(KeyType::ESC)) {
				tryClose();
			} else if (m_joystick->wasKeyDown(KeyType::JUMP)) {
				if (m_allowclose) {
					acceptInput(quit_mode_accept);
					quitMenu();
				}
			}
		}
		return handled;
	}

	return GUIModalMenu::preprocessEvent(event);
}

/******************************************************************************/
bool GUIFormSpecMenu::DoubleClickDetection(const SEvent event)
{
	/* The following code is for capturing double-clicks of the mouse button
	 * and translating the double-click into an EET_KEY_INPUT_EVENT event
	 * -- which closes the form -- under some circumstances.
	 *
	 * There have been many github issues reporting this as a bug even though it
	 * was an intended feature.  For this reason, remapping the double-click as
	 * an ESC must be explicitly set when creating this class via the
	 * /p remap_dbl_click parameter of the constructor.
	 */

	if (!m_remap_dbl_click)
		return false;

	if (event.MouseInput.Event == EMIE_LMOUSE_PRESSED_DOWN) {
		m_doubleclickdetect[0].pos  = m_doubleclickdetect[1].pos;
		m_doubleclickdetect[0].time = m_doubleclickdetect[1].time;

		m_doubleclickdetect[1].pos  = m_pointer;
		m_doubleclickdetect[1].time = porting::getTimeMs();
	}
	else if (event.MouseInput.Event == EMIE_LMOUSE_LEFT_UP) {
		u64 delta = porting::getDeltaMs(m_doubleclickdetect[0].time, porting::getTimeMs());
		if (delta > 400) {
			return false;
		}

		double squaredistance =
				m_doubleclickdetect[0].pos
				.getDistanceFromSQ(m_doubleclickdetect[1].pos);

		if (squaredistance > (30*30)) {
			return false;
		}

		SEvent* translated = new SEvent();
		assert(translated != 0);
		//translate doubleclick to escape
		memset(translated, 0, sizeof(SEvent));
		translated->EventType = irr::EET_KEY_INPUT_EVENT;
		translated->KeyInput.Key         = KEY_ESCAPE;
		translated->KeyInput.Control     = false;
		translated->KeyInput.Shift       = false;
		translated->KeyInput.PressedDown = true;
		translated->KeyInput.Char        = 0;
		OnEvent(*translated);

		// no need to send the key up event as we're already deleted
		// and no one else did notice this event
		delete translated;
		return true;
	}

	return false;
}

void GUIFormSpecMenu::tryClose()
{
	if (m_allowclose) {
		doPause = false;
		acceptInput(quit_mode_cancel);
		quitMenu();
	} else {
		m_text_dst->gotText(L"MenuQuit");
	}
}

enum ButtonEventType : u8
{
	BET_LEFT,
	BET_RIGHT,
	BET_MIDDLE,
	BET_WHEEL_UP,
	BET_WHEEL_DOWN,
	BET_UP,
	BET_DOWN,
	BET_MOVE,
	BET_OTHER
};

bool GUIFormSpecMenu::OnEvent(const SEvent& event)
{
	if (event.EventType==EET_KEY_INPUT_EVENT) {
		KeyPress kp(event.KeyInput);
		if (event.KeyInput.PressedDown && (
				(kp == EscapeKey) || (kp == CancelKey) ||
				((m_client != NULL) && (kp == getKeySetting("keymap_inventory"))))) {
			tryClose();
			return true;
		}

		if (m_client != NULL && event.KeyInput.PressedDown &&
				(kp == getKeySetting("keymap_screenshot"))) {
			m_client->makeScreenshot();
		}
		if (event.KeyInput.PressedDown &&
			(event.KeyInput.Key==KEY_RETURN ||
			 event.KeyInput.Key==KEY_UP ||
			 event.KeyInput.Key==KEY_DOWN)
			) {
			switch (event.KeyInput.Key) {
				case KEY_RETURN:
					current_keys_pending.key_enter = true;
					break;
				case KEY_UP:
					current_keys_pending.key_up = true;
					break;
				case KEY_DOWN:
					current_keys_pending.key_down = true;
					break;
				break;
				default:
					//can't happen at all!
					FATAL_ERROR("Reached a source line that can't ever been reached");
					break;
			}
			if (current_keys_pending.key_enter && m_allowclose) {
				acceptInput(quit_mode_accept);
				quitMenu();
			} else {
				acceptInput();
			}
			return true;
		}

	}

	/* Mouse event other than movement, or crossing the border of inventory
	  field while holding right mouse button
	 */
	if (event.EventType == EET_MOUSE_INPUT_EVENT &&
			(event.MouseInput.Event != EMIE_MOUSE_MOVED ||
			 (event.MouseInput.Event == EMIE_MOUSE_MOVED &&
			  event.MouseInput.isRightPressed() &&
			  getItemAtPos(m_pointer).i != getItemAtPos(m_old_pointer).i))) {

		// Get selected item and hovered/clicked item (s)

		m_old_tooltip_id = -1;
		updateSelectedItem();
		ItemSpec s = getItemAtPos(m_pointer);

		Inventory *inv_selected = NULL;
		Inventory *inv_s = NULL;
		InventoryList *list_s = NULL;

		if (m_selected_item) {
			inv_selected = m_invmgr->getInventory(m_selected_item->inventoryloc);
			sanity_check(inv_selected);
			sanity_check(inv_selected->getList(m_selected_item->listname) != NULL);
		}

		u32 s_count = 0;

		if (s.isValid())
		do { // breakable
			inv_s = m_invmgr->getInventory(s.inventoryloc);

			if (!inv_s) {
				errorstream << "InventoryMenu: The selected inventory location "
						<< "\"" << s.inventoryloc.dump() << "\" doesn't exist"
						<< std::endl;
				s.i = -1;  // make it invalid again
				break;
			}

			list_s = inv_s->getList(s.listname);
			if (list_s == NULL) {
				verbosestream << "InventoryMenu: The selected inventory list \""
						<< s.listname << "\" does not exist" << std::endl;
				s.i = -1;  // make it invalid again
				break;
			}

			if ((u32)s.i >= list_s->getSize()) {
				infostream << "InventoryMenu: The selected inventory list \""
						<< s.listname << "\" is too small (i=" << s.i << ", size="
						<< list_s->getSize() << ")" << std::endl;
				s.i = -1;  // make it invalid again
				break;
			}

			s_count = list_s->getItem(s.i).count;
		} while(0);

		bool identical = m_selected_item && s.isValid() &&
			(inv_selected == inv_s) &&
			(m_selected_item->listname == s.listname) &&
			(m_selected_item->i == s.i);

		ButtonEventType button = BET_LEFT;
		ButtonEventType updown = BET_OTHER;
		switch (event.MouseInput.Event) {
		case EMIE_LMOUSE_PRESSED_DOWN:
			button = BET_LEFT; updown = BET_DOWN;
			break;
		case EMIE_RMOUSE_PRESSED_DOWN:
			button = BET_RIGHT; updown = BET_DOWN;
			break;
		case EMIE_MMOUSE_PRESSED_DOWN:
			button = BET_MIDDLE; updown = BET_DOWN;
			break;
		case EMIE_MOUSE_WHEEL:
			button = (event.MouseInput.Wheel > 0) ?
				BET_WHEEL_UP : BET_WHEEL_DOWN;
			updown = BET_DOWN;
			break;
		case EMIE_LMOUSE_LEFT_UP:
			button = BET_LEFT; updown = BET_UP;
			break;
		case EMIE_RMOUSE_LEFT_UP:
			button = BET_RIGHT; updown = BET_UP;
			break;
		case EMIE_MMOUSE_LEFT_UP:
			button = BET_MIDDLE; updown = BET_UP;
			break;
		case EMIE_MOUSE_MOVED:
			updown = BET_MOVE;
			break;
		default:
			break;
		}

		// Set this number to a positive value to generate a move action
		// from m_selected_item to s.
		u32 move_amount = 0;

		// Set this number to a positive value to generate a move action
		// from s to the next inventory ring.
		u32 shift_move_amount = 0;

		// Set this number to a positive value to generate a drop action
		// from m_selected_item.
		u32 drop_amount = 0;

		// Set this number to a positive value to generate a craft action at s.
		u32 craft_amount = 0;

		switch (updown) {
		case BET_DOWN:
			// Some mouse button has been pressed

			//infostream<<"Mouse button "<<button<<" pressed at p=("
			//	<<p.X<<","<<p.Y<<")"<<std::endl;

			m_selected_dragging = false;

			if (s.isValid() && s.listname == "craftpreview") {
				// Craft preview has been clicked: craft
				craft_amount = (button == BET_MIDDLE ? 10 : 1);
			} else if (!m_selected_item) {
				if (s_count && button != BET_WHEEL_UP) {
					// Non-empty stack has been clicked: select or shift-move it
					m_selected_item = new ItemSpec(s);

					u32 count;
					if (button == BET_RIGHT)
						count = (s_count + 1) / 2;
					else if (button == BET_MIDDLE)
						count = MYMIN(s_count, 10);
					else if (button == BET_WHEEL_DOWN)
						count = 1;
					else  // left
						count = s_count;

					if (!event.MouseInput.Shift) {
						// no shift: select item
						m_selected_amount = count;
						m_selected_dragging = button != BET_WHEEL_DOWN;
						m_auto_place = false;
					} else {
						// shift pressed: move item, right click moves 1
						shift_move_amount = button == BET_RIGHT ? 1 : count;
					}
				}
			} else { // m_selected_item != NULL
				assert(m_selected_amount >= 1);

				if (s.isValid()) {
					// Clicked a slot: move
					if (button == BET_RIGHT || button == BET_WHEEL_UP)
						move_amount = 1;
					else if (button == BET_MIDDLE)
						move_amount = MYMIN(m_selected_amount, 10);
					else if (button == BET_LEFT)
						move_amount = m_selected_amount;
					// else wheeldown

					if (identical) {
						if (button == BET_WHEEL_DOWN) {
							if (m_selected_amount < s_count)
								++m_selected_amount;
						} else {
							if (move_amount >= m_selected_amount)
								m_selected_amount = 0;
							else
								m_selected_amount -= move_amount;
							move_amount = 0;
						}
					}
				} else if (!getAbsoluteClippingRect().isPointInside(m_pointer)
						&& button != BET_WHEEL_DOWN) {
					// Clicked outside of the window: drop
					if (button == BET_RIGHT || button == BET_WHEEL_UP)
						drop_amount = 1;
					else if (button == BET_MIDDLE)
						drop_amount = MYMIN(m_selected_amount, 10);
					else  // left
						drop_amount = m_selected_amount;
				}
			}
		break;
		case BET_UP:
			// Some mouse button has been released

			//infostream<<"Mouse button "<<button<<" released at p=("
			//	<<p.X<<","<<p.Y<<")"<<std::endl;

			if (m_selected_dragging && m_selected_item) {
				if (s.isValid()) {
					if (!identical) {
						// Dragged to different slot: move all selected
						move_amount = m_selected_amount;
					}
				} else if (!getAbsoluteClippingRect().isPointInside(m_pointer)) {
					// Dragged outside of window: drop all selected
					drop_amount = m_selected_amount;
				}
			}

			m_selected_dragging = false;
			// Keep track of whether the mouse button be released
			// One click is drag without dropping. Click + release
			// + click changes to drop item when moved mode
			if (m_selected_item)
				m_auto_place = true;
		break;
		case BET_MOVE:
			// Mouse has been moved and rmb is down and mouse pointer just
			// entered a new inventory field (checked in the entry-if, this
			// is the only action here that is generated by mouse movement)
			if (m_selected_item && s.isValid() && s.listname != "craftpreview") {
				// Move 1 item
				// TODO: middle mouse to move 10 items might be handy
				if (m_auto_place) {
					// Only move an item if the destination slot is empty
					// or contains the same item type as what is going to be
					// moved
					InventoryList *list_from = inv_selected->getList(m_selected_item->listname);
					InventoryList *list_to = list_s;
					assert(list_from && list_to);
					ItemStack stack_from = list_from->getItem(m_selected_item->i);
					ItemStack stack_to = list_to->getItem(s.i);
					if (stack_to.empty() || stack_to.name == stack_from.name)
						move_amount = 1;
				}
			}
		break;
		default:
			break;
		}

		// Possibly send inventory action to server
		if (move_amount > 0) {
			// Send IAction::Move

			assert(m_selected_item && m_selected_item->isValid());
			assert(s.isValid());

			assert(inv_selected && inv_s);
			InventoryList *list_from = inv_selected->getList(m_selected_item->listname);
			InventoryList *list_to = list_s;
			assert(list_from && list_to);
			ItemStack stack_from = list_from->getItem(m_selected_item->i);
			ItemStack stack_to = list_to->getItem(s.i);

			// Check how many items can be moved
			move_amount = stack_from.count = MYMIN(move_amount, stack_from.count);
			ItemStack leftover = stack_to.addItem(stack_from, m_client->idef());
			bool move = true;
			// If source stack cannot be added to destination stack at all,
			// they are swapped
			if (leftover.count == stack_from.count &&
					leftover.name == stack_from.name) {

				if (m_selected_swap.empty()) {
					m_selected_amount = stack_to.count;
					m_selected_dragging = false;

					// WARNING: BLACK MAGIC, BUT IN A REDUCED SET
					// Skip next validation checks due async inventory calls
					m_selected_swap = stack_to;
				} else {
					move = false;
				}
			}
			// Source stack goes fully into destination stack
			else if (leftover.empty()) {
				m_selected_amount -= move_amount;
			}
			// Source stack goes partly into destination stack
			else {
				move_amount -= leftover.count;
				m_selected_amount -= move_amount;
			}

			if (move) {
				infostream << "Handing IAction::Move to manager" << std::endl;
				IMoveAction *a = new IMoveAction();
				a->count = move_amount;
				a->from_inv = m_selected_item->inventoryloc;
				a->from_list = m_selected_item->listname;
				a->from_i = m_selected_item->i;
				a->to_inv = s.inventoryloc;
				a->to_list = s.listname;
				a->to_i = s.i;
				m_invmgr->inventoryAction(a);
			}
		} else if (shift_move_amount > 0) {
			u32 mis = m_inventory_rings.size();
			u32 i = 0;
			for (; i < mis; i++) {
				const ListRingSpec &sp = m_inventory_rings[i];
				if (sp.inventoryloc == s.inventoryloc
						&& sp.listname == s.listname)
					break;
			}
			do {
				if (i >= mis) // if not found
					break;
				u32 to_inv_ind = (i + 1) % mis;
				const ListRingSpec &to_inv_sp = m_inventory_rings[to_inv_ind];
				InventoryList *list_from = list_s;
				if (!s.isValid())
					break;
				Inventory *inv_to = m_invmgr->getInventory(to_inv_sp.inventoryloc);
				if (!inv_to)
					break;
				InventoryList *list_to = inv_to->getList(to_inv_sp.listname);
				if (!list_to)
					break;
				ItemStack stack_from = list_from->getItem(s.i);
				assert(shift_move_amount <= stack_from.count);

				infostream << "Handing IAction::Move to manager" << std::endl;
				IMoveAction *a = new IMoveAction();
				a->count = shift_move_amount;
				a->from_inv = s.inventoryloc;
				a->from_list = s.listname;
				a->from_i = s.i;
				a->to_inv = to_inv_sp.inventoryloc;
				a->to_list = to_inv_sp.listname;
				a->move_somewhere = true;
				m_invmgr->inventoryAction(a);
			} while (0);
		} else if (drop_amount > 0) {
			// Send IAction::Drop

			assert(m_selected_item && m_selected_item->isValid());
			assert(inv_selected);
			InventoryList *list_from = inv_selected->getList(m_selected_item->listname);
			assert(list_from);
			ItemStack stack_from = list_from->getItem(m_selected_item->i);

			// Check how many items can be dropped
			drop_amount = stack_from.count = MYMIN(drop_amount, stack_from.count);
			assert(drop_amount > 0 && drop_amount <= m_selected_amount);
			m_selected_amount -= drop_amount;

			infostream << "Handing IAction::Drop to manager" << std::endl;
			IDropAction *a = new IDropAction();
			a->count = drop_amount;
			a->from_inv = m_selected_item->inventoryloc;
			a->from_list = m_selected_item->listname;
			a->from_i = m_selected_item->i;
			m_invmgr->inventoryAction(a);
		} else if (craft_amount > 0) {
			assert(s.isValid());

			// if there are no items selected or the selected item
			// belongs to craftresult list, proceed with crafting
			if (m_selected_item == NULL ||
					!m_selected_item->isValid() || m_selected_item->listname == "craftresult") {

				assert(inv_s);

				// Send IACTION_CRAFT
				infostream << "Handing IACTION_CRAFT to manager" << std::endl;
				ICraftAction *a = new ICraftAction();
				a->count = craft_amount;
				a->craft_inv = s.inventoryloc;
				m_invmgr->inventoryAction(a);
			}
		}

		// If m_selected_amount has been decreased to zero, deselect
		if (m_selected_amount == 0) {
			m_selected_swap.clear();
			delete m_selected_item;
			m_selected_item = NULL;
			m_selected_amount = 0;
			m_selected_dragging = false;
		}
		m_old_pointer = m_pointer;
	}

	if (event.EventType == EET_GUI_EVENT) {
		if (event.GUIEvent.EventType == gui::EGET_TAB_CHANGED
				&& isVisible()) {
			// find the element that was clicked
			for (GUIFormSpecMenu::FieldSpec &s : m_fields) {
				if ((s.ftype == f_TabHeader) &&
						(s.fid == event.GUIEvent.Caller->getID())) {
					s.send = true;
					acceptInput();
					s.send = false;
					return true;
				}
			}
		}
		if (event.GUIEvent.EventType == gui::EGET_ELEMENT_FOCUS_LOST
				&& isVisible()) {
			if (!canTakeFocus(event.GUIEvent.Element)) {
				infostream<<"GUIFormSpecMenu: Not allowing focus change."
						<<std::endl;
				// Returning true disables focus change
				return true;
			}
		}
		if ((event.GUIEvent.EventType == gui::EGET_BUTTON_CLICKED) ||
				(event.GUIEvent.EventType == gui::EGET_CHECKBOX_CHANGED) ||
				(event.GUIEvent.EventType == gui::EGET_COMBO_BOX_CHANGED) ||
				(event.GUIEvent.EventType == gui::EGET_SCROLL_BAR_CHANGED)) {
			unsigned int btn_id = event.GUIEvent.Caller->getID();

			if (btn_id == 257) {
				if (m_allowclose) {
					acceptInput(quit_mode_accept);
					quitMenu();
				} else {
					acceptInput();
					m_text_dst->gotText(L"ExitButton");
				}
				// quitMenu deallocates menu
				return true;
			}

			// find the element that was clicked
			for (GUIFormSpecMenu::FieldSpec &s : m_fields) {
				// if its a button, set the send field so
				// lua knows which button was pressed
				if ((s.ftype == f_Button || s.ftype == f_CheckBox) &&
						s.fid == event.GUIEvent.Caller->getID()) {
					s.send = true;
					if (s.is_exit) {
						if (m_allowclose) {
							acceptInput(quit_mode_accept);
							quitMenu();
						} else {
							m_text_dst->gotText(L"ExitButton");
						}
						return true;
					}

					acceptInput(quit_mode_no);
					s.send = false;
					return true;

				} else if ((s.ftype == f_DropDown) &&
						(s.fid == event.GUIEvent.Caller->getID())) {
					// only send the changed dropdown
					for (GUIFormSpecMenu::FieldSpec &s2 : m_fields) {
						if (s2.ftype == f_DropDown) {
							s2.send = false;
						}
					}
					s.send = true;
					acceptInput(quit_mode_no);

					// revert configuration to make sure dropdowns are sent on
					// regular button click
					for (GUIFormSpecMenu::FieldSpec &s2 : m_fields) {
						if (s2.ftype == f_DropDown) {
							s2.send = true;
						}
					}
					return true;
				} else if ((s.ftype == f_ScrollBar) &&
						(s.fid == event.GUIEvent.Caller->getID())) {
					s.fdefault = L"Changed";
					acceptInput(quit_mode_no);
					s.fdefault = L"";
				} else if ((s.ftype == f_Unknown) &&
						(s.fid == event.GUIEvent.Caller->getID())) {
					s.send = true;
					acceptInput();
					s.send = false;
				}
			}
		}

		if (event.GUIEvent.EventType == gui::EGET_EDITBOX_ENTER) {
			if (event.GUIEvent.Caller->getID() > 257) {
				bool close_on_enter = true;
				for (GUIFormSpecMenu::FieldSpec &s : m_fields) {
					if (s.ftype == f_Unknown &&
							s.fid == event.GUIEvent.Caller->getID()) {
						current_field_enter_pending = s.fname;
						std::unordered_map<std::string, bool>::const_iterator it =
							field_close_on_enter.find(s.fname);
						if (it != field_close_on_enter.end())
							close_on_enter = (*it).second;

						break;
					}
				}

				if (m_allowclose && close_on_enter) {
					current_keys_pending.key_enter = true;
					acceptInput(quit_mode_accept);
					quitMenu();
				} else {
					current_keys_pending.key_enter = true;
					acceptInput();
				}
				// quitMenu deallocates menu
				return true;
			}
		}

		if (event.GUIEvent.EventType == gui::EGET_TABLE_CHANGED) {
			int current_id = event.GUIEvent.Caller->getID();
			if (current_id > 257) {
				// find the element that was clicked
				for (GUIFormSpecMenu::FieldSpec &s : m_fields) {
					// if it's a table, set the send field
					// so lua knows which table was changed
					if ((s.ftype == f_Table) && (s.fid == current_id)) {
						s.send = true;
						acceptInput();
						s.send=false;
					}
				}
				return true;
			}
		}
	}

	return Parent ? Parent->OnEvent(event) : false;
}

/**
 * get name of element by element id
 * @param id of element
 * @return name string or empty string
 */
std::string GUIFormSpecMenu::getNameByID(s32 id)
{
	for (FieldSpec &spec : m_fields) {
		if (spec.fid == id) {
			return spec.fname;
		}
	}
	return "";
}

/**
 * get label of element by id
 * @param id of element
 * @return label string or empty string
 */
std::wstring GUIFormSpecMenu::getLabelByID(s32 id)
{
	for (FieldSpec &spec : m_fields) {
		if (spec.fid == id) {
			return spec.flabel;
		}
	}
	return L"";
}

StyleSpec GUIFormSpecMenu::getStyleForElement(const std::string &type,
		const std::string &name, const std::string &parent_type) {
	StyleSpec ret;

	if (!parent_type.empty()) {
		auto it = theme_by_type.find(parent_type);
		if (it != theme_by_type.end()) {
			ret |= it->second;
		}
	}

	auto it = theme_by_type.find(type);
	if (it != theme_by_type.end()) {
		ret |= it->second;
	}

	it = theme_by_name.find(name);
	if (it != theme_by_name.end()) {
		ret |= it->second;
	}

	return ret;
}
