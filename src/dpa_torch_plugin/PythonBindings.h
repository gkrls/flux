#ifndef DPA_TORCH_PLUGIN_PYTHON_BINDINGS_H
#define DPA_TORCH_PLUGIN_PYTHON_BINDINGS_H

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <sstream>

namespace py = pybind11;

template <typename T, typename... Bases> void make_dict_like(py::class_<T, Bases...> &cls) {
  // __repr__ for nice string representation
  // cls.def("__repr__", [](const T &self) -> std::string {
  //   py::dict d;
  //   py::object py_self = py::cast(self);
  //   py::object cls_obj = py_self.attr("__class__");

  //   for (auto item : cls_obj.attr("__dict__").cast<py::dict>()) {
  //     std::string key = py::str(item.first);
  //     if (key.empty() || key[0] == '_' || py::isinstance<py::function>(item.second)) continue;
  //     try {
  //       d[key.c_str()] = py_self.attr(key.c_str());
  //     } catch (...) {}
  //   }

  //   std::string class_name = py::str(cls_obj.attr("__name__"));
  //   std::string dict_repr = py::str(d);
  //   return class_name + "(" + dict_repr + ")";
  // });
  cls.def("__repr__", [](const T &self) -> std::string {
    py::object py_self = py::cast(self);
    py::object cls_obj = py_self.attr("__class__");
    std::string class_name = py::str(cls_obj.attr("__name__"));

    std::string body;
    bool first = true;
    for (auto item : cls_obj.attr("__dict__").cast<py::dict>()) {
      std::string key = py::str(item.first);
      if (key.empty() || key[0] == '_' || py::isinstance<py::function>(item.second)) continue;
      try {
        py::object v = py_self.attr(key.c_str());
        if (!first) body += ", ";
        body += key + "=" + std::string(py::repr(v));
        first = false;
      } catch (...) {}
    }
    return class_name + "(" + body + ")";
  });

  // __getitem__ - access with obj['key']
  cls.def("__getitem__", [](const T &self, const std::string &key) -> py::object {
    py::object py_self = py::cast(self);
    if (!py::hasattr(py_self, key.c_str())) { throw py::key_error("Key '" + key + "' not found"); }
    return py_self.attr(key.c_str());
  });

  // __setitem__ - set with obj['key'] = value
  cls.def("__setitem__", [](T &self, const std::string &key, py::object value) {
    py::object py_self = py::cast(self, py::return_value_policy::reference);
    if (!py::hasattr(py_self, key.c_str())) { throw py::key_error("Key '" + key + "' not found"); }
    try {
      py::setattr(py_self, key.c_str(), value);
    } catch (const py::error_already_set &e) { throw py::type_error("Cannot set attribute '" + key + "'"); }
  });

  // __contains__ - 'key' in obj
  cls.def("__contains__", [](const T &self, const std::string &key) -> bool {
    py::object py_self = py::cast(self);
    return py::hasattr(py_self, key.c_str());
  });

  // get() method with default value
  cls.def(
      "get",
      [](const T &self, const std::string &key, py::object default_value) -> py::object {
        py::object py_self = py::cast(self);
        if (py::hasattr(py_self, key.c_str())) {
          try {
            return py_self.attr(key.c_str());
          } catch (...) { return default_value; }
        }
        return default_value;
      },
      py::arg("key"), py::arg("default") = py::none());

  // keys() method
  cls.def("keys", [](const T &self) -> py::list {
    py::list keys;
    py::object py_self = py::cast(self);
    py::object cls_obj = py_self.attr("__class__");

    for (auto item : cls_obj.attr("__dict__").cast<py::dict>()) {
      std::string key = py::str(item.first);
      if (key.empty() || key[0] == '_' || py::isinstance<py::function>(item.second)) continue;

      if (py::hasattr(py_self, key.c_str())) { keys.append(key); }
    }
    return keys;
  });

  // values() method
  cls.def("values", [](const T &self) -> py::list {
    py::list values;
    py::object py_self = py::cast(self);
    py::object cls_obj = py_self.attr("__class__");

    for (auto item : cls_obj.attr("__dict__").cast<py::dict>()) {
      std::string key = py::str(item.first);
      if (key.empty() || key[0] == '_' || py::isinstance<py::function>(item.second)) continue;

      try {
        if (py::hasattr(py_self, key.c_str())) { values.append(py_self.attr(key.c_str())); }
      } catch (...) {}
    }
    return values;
  });

  // items() method
  cls.def("items", [](const T &self) -> py::list {
    py::list items;
    py::object py_self = py::cast(self);
    py::object cls_obj = py_self.attr("__class__");

    for (auto item : cls_obj.attr("__dict__").cast<py::dict>()) {
      std::string key = py::str(item.first);
      if (key.empty() || key[0] == '_' || py::isinstance<py::function>(item.second)) continue;

      try {
        if (py::hasattr(py_self, key.c_str())) {
          py::object value = py_self.attr(key.c_str());
          items.append(py::make_tuple(key, value));
        }
      } catch (...) {}
    }
    return items;
  });

  // update() method - update from dict or other object
  cls.def("update", [](T &self, py::dict updates) {
    py::object py_self = py::cast(self, py::return_value_policy::reference);

    for (auto item : updates) {
      std::string key = py::str(item.first);
      if (py::hasattr(py_self, key.c_str())) {
        try {
          py::setattr(py_self, key.c_str(), item.second);
        } catch (...) {
          // Skip attributes that can't be set
        }
      }
    }
  });

  // to_dict() method for conversion
  cls.def("to_dict", [](const T &self) -> py::dict {
    py::dict result;
    py::object py_self = py::cast(self);
    py::object cls_obj = py_self.attr("__class__");

    for (auto item : cls_obj.attr("__dict__").cast<py::dict>()) {
      std::string key = py::str(item.first);
      if (key.empty() || key[0] == '_' || py::isinstance<py::function>(item.second)) continue;

      try {
        if (py::hasattr(py_self, key.c_str())) { result[key.c_str()] = py_self.attr(key.c_str()); }
      } catch (...) {}
    }
    return result;
  });
}


#endif