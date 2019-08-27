/*
 * This source file is part of RmlUi, the HTML/CSS Interface Middleware
 *
 * For the latest information, see http://github.com/mikke89/RmlUi
 *
 * Copyright (c) 2008-2010 CodePoint Ltd, Shift Technology Ltd
 * Copyright (c) 2019 The RmlUi Team, and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "Plugin.h"
#include "../../Include/RmlUi/Core/Types.h"
#include "../../Include/RmlUi/Core.h"
#include "ElementContextHook.h"
#include "ElementInfo.h"
#include "ElementLog.h"
#include "FontSource.h"
#include "Geometry.h"
#include "MenuSource.h"
#include "SystemInterface.h"
#include <stack>

namespace Rml {
namespace Debugger {

Plugin* Plugin::instance = nullptr;

Plugin::Plugin()
{
	RMLUI_ASSERT(instance == nullptr);
	instance = this;
	host_context = nullptr;
	debug_context = nullptr;
	log_interface = nullptr;

	menu_element = nullptr;
	info_element = nullptr;
	log_element = nullptr;
	hook_element = nullptr;

	render_outlines = false;

	application_interface = nullptr;
}

Plugin::~Plugin()
{
	instance = nullptr;
}

// Initialises the debugging tools into the given context.
bool Plugin::Initialise(Core::Context* context)
{
	host_context = context;
	Geometry::SetContext(context);

	if (!LoadFont())
	{
		Core::Log::Message(Core::Log::LT_ERROR, "Failed to initialise debugger, unable to load font.");
		return false;
	}

	if (!LoadMenuElement() ||
		!LoadInfoElement() ||
		!LoadLogElement())
	{
		Core::Log::Message(Core::Log::LT_ERROR, "Failed to initialise debugger, error while load debugger elements.");
		return false;
	}

	hook_element_instancer = std::make_unique< Core::ElementInstancerGeneric<ElementContextHook> >();
	Core::Factory::RegisterElementInstancer("debug-hook", hook_element_instancer.get());

	return true;
}

// Sets the context to be debugged.
bool Plugin::SetContext(Core::Context* context)
{
	// Remove the debug hook from the old context.
	if (debug_context && hook_element)
	{
		debug_context->UnloadDocument(hook_element);
		hook_element = nullptr;
	}

	// Add the debug hook into the new context.
	if (context)
	{
		Core::ElementDocument* element = context->CreateDocument("debug-hook");
		if (!element)
			return false;

		RMLUI_ASSERT(!hook_element);
		hook_element = dynamic_cast< ElementContextHook* >(element);
		if (!hook_element)
		{
			context->UnloadDocument(element);
			return false;
		}

		hook_element->Initialise(this);
	}

	// Attach the info element to the new context.
	if (info_element)
	{
		if (debug_context)
		{
			debug_context->RemoveEventListener("click", info_element, true);
			debug_context->RemoveEventListener("mouseover", info_element, true);
		}

		if (context)
		{
			context->AddEventListener("click", info_element, true);
			context->AddEventListener("mouseover", info_element, true);
		}

		info_element->Reset();
	}

	debug_context = context;
	return true;
}

// Sets the visibility of the debugger.
void Plugin::SetVisible(bool visibility)
{
	if (visibility)
		menu_element->SetProperty(Core::PropertyId::Visibility, Core::Property(Core::Style::Visibility::Visible));
	else
		menu_element->SetProperty(Core::PropertyId::Visibility, Core::Property(Core::Style::Visibility::Hidden));
}

// Returns the visibility of the debugger.
bool Plugin::IsVisible()
{
	return menu_element->IsVisible();
}

// Renders any debug elements in the debug context.
void Plugin::Render()
{
	// Render the outlines of the debug context's elements.
	if (render_outlines && debug_context)
	{
		for (int i = 0; i < debug_context->GetNumDocuments(); ++i)
		{
			Core::ElementDocument* document = debug_context->GetDocument(i);
			if (document->GetId().find("rmlui-debug-") == 0)
				continue;

			std::stack< Core::Element* > element_stack;
			element_stack.push(document);

			while (!element_stack.empty())
			{
				Core::Element* element = element_stack.top();
				element_stack.pop();
				if (element->IsVisible())
				{
					Core::ElementUtilities::ApplyTransform(*element);
					for (int j = 0; j < element->GetNumBoxes(); ++j)
					{
						const Core::Box& box = element->GetBox(j);
						Geometry::RenderOutline(
							element->GetAbsoluteOffset(Core::Box::BORDER) + box.GetPosition(Core::Box::BORDER), 
							box.GetSize(Core::Box::BORDER), 
							Core::Colourb(255, 0, 0, 128), 
							1
						);
					}

					for (int j = 0; j < element->GetNumChildren(); ++j)
						element_stack.push(element->GetChild(j));
				}
			}
		}
	}

	// Render the info element's boxes.
	if (info_element && info_element->IsVisible())
	{
		info_element->RenderHoverElement();
		info_element->RenderSourceElement();
	}
}

// Called when RmlUi shuts down.
void Plugin::OnShutdown()
{
	// Release the elements before we leak track, this ensures the debugger hook has been cleared
	// and that we don't try send the messages to the debug log window
	ReleaseElements();

	hook_element_instancer.reset();

	delete this;
}

// Called whenever a RmlUi context is destroyed.
void Plugin::OnContextDestroy(Core::Context* context)
{
	if (context == debug_context)
	{
		// The context we're debugging is being destroyed, so we need to remove our debug hook elements.
		SetContext(nullptr);
	}

	if (context == host_context)
	{
		// Our host is being destroyed, so we need to shut down the debugger.

		ReleaseElements();

		Geometry::SetContext(nullptr);
		host_context = nullptr;
	}
}

// Called whenever an element is destroyed.
void Plugin::OnElementDestroy(Core::Element* element)
{
	if (info_element)
		info_element->OnElementDestroy(element);
}

// Event handler for events from the debugger elements.
void Plugin::ProcessEvent(Core::Event& event)
{
	if (event == Core::EventId::Click)
	{
		if (event.GetTargetElement()->GetId() == "event-log-button")
		{
			if (log_element->IsVisible())
				log_element->SetProperty(Core::PropertyId::Visibility, Core::Property(Core::Style::Visibility::Hidden));
			else
				log_element->SetProperty(Core::PropertyId::Visibility, Core::Property(Core::Style::Visibility::Visible));
		}
		else if (event.GetTargetElement()->GetId() == "debug-info-button")
		{
			if (info_element->IsVisible())
				info_element->SetProperty(Core::PropertyId::Visibility, Core::Property(Core::Style::Visibility::Hidden));
			else
				info_element->SetProperty(Core::PropertyId::Visibility, Core::Property(Core::Style::Visibility::Visible));
		}
		else if (event.GetTargetElement()->GetId() == "outlines-button")
		{
			render_outlines = !render_outlines;
		}
	}
}

Plugin* Plugin::GetInstance()
{
	return instance;
}

bool Plugin::LoadFont()
{
	return (Core::FontDatabase::LoadFontFace(Core::FontDatabase::FreeType, lacuna_regular, sizeof(lacuna_regular) / sizeof(unsigned char), "Lacuna", Core::Style::FontStyle::Normal, Core::Style::FontWeight::Normal) &&
			Core::FontDatabase::LoadFontFace(Core::FontDatabase::FreeType, lacuna_italic, sizeof(lacuna_italic) / sizeof(unsigned char), "Lacuna", Core::Style::FontStyle::Italic, Core::Style::FontWeight::Normal));
}

bool Plugin::LoadMenuElement()
{
	menu_element = host_context->CreateDocument();
	if (!menu_element)
		return false;

	menu_element->SetId("rmlui-debug-menu");
	menu_element->SetProperty(Core::PropertyId::Visibility, Core::Property(Core::Style::Visibility::Hidden));
	menu_element->SetInnerRML(menu_rml);

	Core::SharedPtr<Core::StyleSheet> style_sheet = Core::Factory::InstanceStyleSheetString(menu_rcss);
	if (!style_sheet)
	{
		host_context->UnloadDocument(menu_element);
		menu_element = nullptr;
		return false;
	}

	menu_element->SetStyleSheet(std::move(style_sheet));

	// Set the version info in the menu.
	menu_element->GetElementById("version-number")->SetInnerRML("v" + Rml::Core::GetVersion());

	// Attach to the buttons.
	Core::Element* event_log_button = menu_element->GetElementById("event-log-button");
	event_log_button->AddEventListener(Rml::Core::EventId::Click, this);

	Core::Element* element_info_button = menu_element->GetElementById("debug-info-button");
	element_info_button->AddEventListener(Rml::Core::EventId::Click, this);

	Core::Element* outlines_button = menu_element->GetElementById("outlines-button");
	outlines_button->AddEventListener(Rml::Core::EventId::Click, this);

	return true;
}

bool Plugin::LoadInfoElement()
{
	info_element_instancer = std::make_unique< Core::ElementInstancerGeneric<ElementInfo> >();
	Core::Factory::RegisterElementInstancer("debug-info", info_element_instancer.get());
	info_element = dynamic_cast< ElementInfo* >(host_context->CreateDocument("debug-info"));
	if (!info_element)
		return false;

	info_element->SetProperty(Core::PropertyId::Visibility, Core::Property(Core::Style::Visibility::Hidden));

	if (!info_element->Initialise())
	{
		host_context->UnloadDocument(info_element);
		info_element = nullptr;

		return false;
	}

	return true;
}

bool Plugin::LoadLogElement()
{
	log_element_instancer = std::make_unique< Core::ElementInstancerGeneric<ElementLog> >();
	Core::Factory::RegisterElementInstancer("debug-log", log_element_instancer.get());
	log_element = dynamic_cast< ElementLog* >(host_context->CreateDocument("debug-log"));
	if (!log_element)
		return false;

	log_element->SetProperty(Core::PropertyId::Visibility, Core::Property(Core::Style::Visibility::Hidden));

	if (!log_element->Initialise())
	{
		host_context->UnloadDocument(log_element);
		log_element = nullptr;

		return false;
	}

	// Make the system interface; this will trap the log messages for us.
	application_interface = Core::GetSystemInterface();
	log_interface = std::make_unique<SystemInterface>(application_interface, log_element);
	Core::SetSystemInterface(log_interface.get());

	return true;
}

void Plugin::ReleaseElements()
{
	if (host_context)
	{
		if (menu_element)
		{
			host_context->UnloadDocument(menu_element);
			menu_element = nullptr;
		}

		if (info_element)
		{
			host_context->UnloadDocument(info_element);
			info_element = nullptr;
		}

		if (log_element)
		{
			host_context->UnloadDocument(log_element);
			log_element = nullptr;
			Core::SetSystemInterface(application_interface);
			application_interface = nullptr;
			log_interface.reset();
		}
	}

	if (debug_context)
	{
		if (hook_element)
		{
			debug_context->UnloadDocument(hook_element);
			hook_element = nullptr;
		}
	}
}

}
}
