// cl /nologo xmltest.cc ole32.lib oleaut32.lib comsuppw.lib
#include <msxml6.h>
#include <comdef.h>
#include <comip.h>
#include <comutil.h>
#include <stdio.h>
#include <wrl/client.h>

// COM progression:
// 1. c-style
// 2. with atl helpers (CComPtr, CComBSTR)
// 3. with compiler helpers (bstr_t, _com_ptr_t) (exceptions!)
// 4. with _COM_SMARTPTR_TYPEDEF / Ptr types (comdef.h) (exceptions!)
// 5. with #import and Ptr types (comdef.h) (exceptions!)
// 6. ATL-less Microsoft::WRL::ComPtr, manual error handling again

#if 0
  # With #import <msxml.dll>, can do the CHK_HR-less, exception-using version:
  MSXML2::IXMLDOMDocumentPtr  p;  // this is different from ::IXMLDOMDocumentPtr!
  p.CreateInstance(__uuidof(MSXML2::DOMDocument60));
  p->load("stocks.xml");
  p->documentElement->nodeName;
#endif

using Microsoft::WRL::ComPtr;

#define CHK_HR(x) do { if (FAILED(x)) exit(1); } while (0)

void print(IXMLDOMNode* n, int indent) {
  DOMNodeType node_type;
  CHK_HR(n->get_nodeType(&node_type));
  bstr_t node_name, namespace_uri;
  CHK_HR(n->get_nodeName(node_name.GetAddress()));
  CHK_HR(n->get_namespaceURI(namespace_uri.GetAddress()));
  for (int i = 0; i < indent; ++i) printf(" ");
  printf("%d %S %S", node_type, (BSTR)node_name, (BSTR)namespace_uri);
  if (node_type == NODE_ELEMENT) {
    ComPtr<IXMLDOMElement> ChildElt;
    // Note: Can't just upcast COM object, need to QueryInterface().
    CHK_HR(n->QueryInterface(ChildElt.GetAddressOf()));
    bstr_t tag_name;
    CHK_HR(ChildElt->get_tagName(tag_name.GetAddress()));
    printf(" %S", (BSTR)tag_name);
  }
  printf("\n");

  ComPtr<IXMLDOMNodeList> children;
  long num_children;
  CHK_HR(n->get_childNodes(children.GetAddressOf()));
  CHK_HR(children->get_length(&num_children));
  for (long i = 0; i < num_children; ++i) {
    ComPtr<IXMLDOMNode> child;
    CHK_HR(children->get_item(i, child.GetAddressOf()));
    print(child.Get(), indent + 2);
  }
}

void DumpFile(const char* filename) {
  ComPtr<IXMLDOMDocument> xml_doc;
  CHK_HR(CoCreateInstance(__uuidof(DOMDocument60), NULL,
     CLSCTX_INPROC_SERVER, IID_PPV_ARGS(xml_doc.GetAddressOf())));
  xml_doc->put_async(VARIANT_FALSE);
  xml_doc->put_validateOnParse(VARIANT_FALSE);
  xml_doc->put_resolveExternals(VARIANT_FALSE);
  VARIANT_BOOL load_success;
  CHK_HR(xml_doc->load(variant_t(filename), &load_success));
  if (load_success != VARIANT_TRUE) {
    // Failed to load xml, get last parsing error
    ComPtr<IXMLDOMParseError> parse_err;
    CHK_HR(xml_doc->get_parseError(parse_err.GetAddressOf()));
    bstr_t reason;
    CHK_HR(parse_err->get_reason(reason.GetAddress()));
    printf("Failed to load DOM from stocks.xml. %S\n", (BSTR)reason);
    return;
  }
  ComPtr<IXMLDOMElement> root;
  CHK_HR(xml_doc->get_documentElement(root.GetAddressOf()));
  print(root.Get(), 0);
}

int main(int argc, char *argv[]) {
  CHK_HR(CoInitialize(NULL));
  for (int i = 1; i < argc; ++i)
    DumpFile(argv[i]);
  CoUninitialize();
}