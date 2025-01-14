// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/execution/arguments-inl.h"
#include "src/heap/heap-inl.h"
#include "src/numbers/conversions.h"
#include "src/objects/js-array-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/slots.h"
#include "src/objects/smi.h"
#include "src/strings/string-builder-inl.h"
#include "src/strings/unicode-inl.h"

#if V8_ENABLE_WEBASSEMBLY
// TODO(chromium:1236668): Drop this when the "SaveAndClearThreadInWasmFlag"
// approach is no longer needed.
#include "src/trap-handler/trap-handler.h"
#endif  // V8_ENABLE_WEBASSEMBLY

namespace v8 {
namespace internal {

namespace {

#if V8_ENABLE_WEBASSEMBLY
class V8_NODISCARD SaveAndClearThreadInWasmFlag {
 public:
  explicit SaveAndClearThreadInWasmFlag(Isolate* isolate) : isolate_(isolate) {
    if (trap_handler::IsTrapHandlerEnabled()) {
      if (trap_handler::IsThreadInWasm()) {
        thread_was_in_wasm_ = true;
        trap_handler::ClearThreadInWasm();
      }
    }
  }
  ~SaveAndClearThreadInWasmFlag() {
    if (thread_was_in_wasm_ && !isolate_->has_pending_exception()) {
      trap_handler::SetThreadInWasm();
    }
  }

 private:
  bool thread_was_in_wasm_{false};
  Isolate* isolate_;
};
#define CLEAR_THREAD_IN_WASM_SCOPE \
  SaveAndClearThreadInWasmFlag non_wasm_scope(isolate)
#else
#define CLEAR_THREAD_IN_WASM_SCOPE (void)0
#endif  // V8_ENABLE_WEBASSEMBLY

}  // namespace

RUNTIME_FUNCTION(Runtime_GetSubstitution) {
  HandleScope scope(isolate);
  DCHECK_EQ(5, args.length());
  Handle<String> matched = args.at<String>(0);
  Handle<String> subject = args.at<String>(1);
  int position = args.smi_value_at(2);
  Handle<String> replacement = args.at<String>(3);
  int start_index = args.smi_value_at(4);

  // A simple match without captures.
  class SimpleMatch : public String::Match {
   public:
    SimpleMatch(Handle<String> match, Handle<String> prefix,
                Handle<String> suffix)
        : match_(match), prefix_(prefix), suffix_(suffix) {}

    Handle<String> GetMatch() override { return match_; }
    Handle<String> GetPrefix() override { return prefix_; }
    Handle<String> GetSuffix() override { return suffix_; }

    int CaptureCount() override { return 0; }
    bool HasNamedCaptures() override { return false; }
    MaybeHandle<String> GetCapture(int i, bool* capture_exists) override {
      *capture_exists = false;
      return match_;  // Return arbitrary string handle.
    }
    MaybeHandle<String> GetNamedCapture(Handle<String> name,
                                        CaptureState* state) override {
      UNREACHABLE();
    }

   private:
    Handle<String> match_, prefix_, suffix_;
  };

  Handle<String> prefix =
      isolate->factory()->NewSubString(subject, 0, position);
  Handle<String> suffix = isolate->factory()->NewSubString(
      subject, position + matched->length(), subject->length());
  SimpleMatch match(matched, prefix, suffix);

  RETURN_RESULT_OR_FAILURE(
      isolate,
      String::GetSubstitution(isolate, &match, replacement, start_index));
}

// This may return an empty MaybeHandle if an exception is thrown or
// we abort due to reaching the recursion limit.
MaybeHandle<String> StringReplaceOneCharWithString(
    Isolate* isolate, Handle<String> subject, Handle<String> search,
    Handle<String> replace, bool* found, int recursion_limit) {
  StackLimitCheck stackLimitCheck(isolate);
  if (stackLimitCheck.HasOverflowed() || (recursion_limit == 0)) {
    return MaybeHandle<String>();
  }
  recursion_limit--;
  if (subject->IsConsString()) {
    ConsString cons = ConsString::cast(*subject);
    Handle<String> first = handle(cons.first(), isolate);
    Handle<String> second = handle(cons.second(), isolate);
    Handle<String> new_first;
    if (!StringReplaceOneCharWithString(isolate, first, search, replace, found,
                                        recursion_limit).ToHandle(&new_first)) {
      return MaybeHandle<String>();
    }
    if (*found) return isolate->factory()->NewConsString(new_first, second);

    Handle<String> new_second;
    if (!StringReplaceOneCharWithString(isolate, second, search, replace, found,
                                        recursion_limit)
             .ToHandle(&new_second)) {
      return MaybeHandle<String>();
    }
    if (*found) return isolate->factory()->NewConsString(first, new_second);

    return subject;
  } else {
    int index = String::IndexOf(isolate, subject, search, 0);
    if (index == -1) return subject;
    *found = true;
    Handle<String> first = isolate->factory()->NewSubString(subject, 0, index);
    Handle<String> cons1;
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, cons1, isolate->factory()->NewConsString(first, replace),
        String);
    Handle<String> second =
        isolate->factory()->NewSubString(subject, index + 1, subject->length());
    return isolate->factory()->NewConsString(cons1, second);
  }
}

RUNTIME_FUNCTION(Runtime_StringReplaceOneCharWithString) {
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  Handle<String> subject = args.at<String>(0);
  Handle<String> search = args.at<String>(1);
  Handle<String> replace = args.at<String>(2);

  // If the cons string tree is too deep, we simply abort the recursion and
  // retry with a flattened subject string.
  const int kRecursionLimit = 0x1000;
  bool found = false;
  Handle<String> result;
  if (StringReplaceOneCharWithString(isolate, subject, search, replace, &found,
                                     kRecursionLimit).ToHandle(&result)) {
    return *result;
  }
  if (isolate->has_pending_exception())
    return ReadOnlyRoots(isolate).exception();

  subject = String::Flatten(isolate, subject);
  if (StringReplaceOneCharWithString(isolate, subject, search, replace, &found,
                                     kRecursionLimit).ToHandle(&result)) {
    return *result;
  }
  if (isolate->has_pending_exception())
    return ReadOnlyRoots(isolate).exception();
  // In case of empty handle and no pending exception we have stack overflow.
  return isolate->StackOverflow();
}

RUNTIME_FUNCTION(Runtime_StringLastIndexOf) {
  HandleScope handle_scope(isolate);
  return String::LastIndexOf(isolate, args.at(0), args.at(1),
                             isolate->factory()->undefined_value());
}

RUNTIME_FUNCTION(Runtime_StringSubstring) {
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  Handle<String> string = args.at<String>(0);
  int start = args.smi_value_at(1);
  int end = args.smi_value_at(2);
  DCHECK_LE(0, start);
  DCHECK_LE(start, end);
  DCHECK_LE(end, string->length());
  return *isolate->factory()->NewSubString(string, start, end);
}

RUNTIME_FUNCTION(Runtime_StringAdd) {
  // This is used by Wasm stringrefs.
  CLEAR_THREAD_IN_WASM_SCOPE;
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  Handle<String> str1 = args.at<String>(0);
  Handle<String> str2 = args.at<String>(1);
  RETURN_RESULT_OR_FAILURE(isolate,
                           isolate->factory()->NewConsString(str1, str2));
}


RUNTIME_FUNCTION(Runtime_InternalizeString) {
  HandleScope handles(isolate);
  DCHECK_EQ(1, args.length());
  Handle<String> string = args.at<String>(0);
  return *isolate->factory()->InternalizeString(string);
}

RUNTIME_FUNCTION(Runtime_StringCharCodeAt) {
  HandleScope handle_scope(isolate);
  DCHECK_EQ(2, args.length());

  Handle<String> subject = args.at<String>(0);
  uint32_t i = NumberToUint32(args[1]);

  // Flatten the string.  If someone wants to get a char at an index
  // in a cons string, it is likely that more indices will be
  // accessed.
  subject = String::Flatten(isolate, subject);

  if (i >= static_cast<uint32_t>(subject->length())) {
    return ReadOnlyRoots(isolate).nan_value();
  }

  return Smi::FromInt(subject->Get(i));
}

RUNTIME_FUNCTION(Runtime_StringBuilderConcat) {
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  Handle<JSArray> array = args.at<JSArray>(0);
  int32_t array_length;
  if (!args[1].ToInt32(&array_length)) {
    THROW_NEW_ERROR_RETURN_FAILURE(isolate, NewInvalidStringLengthError());
  }
  Handle<String> special = args.at<String>(2);

  size_t actual_array_length = 0;
  CHECK(TryNumberToSize(array->length(), &actual_array_length));
  CHECK_GE(array_length, 0);
  CHECK(static_cast<size_t>(array_length) <= actual_array_length);

  // This assumption is used by the slice encoding in one or two smis.
  DCHECK_GE(Smi::kMaxValue, String::kMaxLength);

  CHECK(array->HasFastElements());
  JSObject::EnsureCanContainHeapObjectElements(array);

  int special_length = special->length();
  if (!array->HasObjectElements()) {
    return isolate->Throw(ReadOnlyRoots(isolate).illegal_argument_string());
  }

  int length;
  bool one_byte = special->IsOneByteRepresentation();

  {
    DisallowGarbageCollection no_gc;
    FixedArray fixed_array = FixedArray::cast(array->elements());
    if (fixed_array.length() < array_length) {
      array_length = fixed_array.length();
    }

    if (array_length == 0) {
      return ReadOnlyRoots(isolate).empty_string();
    } else if (array_length == 1) {
      Object first = fixed_array.get(0);
      if (first.IsString()) return first;
    }
    length = StringBuilderConcatLength(special_length, fixed_array,
                                       array_length, &one_byte);
  }

  if (length == -1) {
    return isolate->Throw(ReadOnlyRoots(isolate).illegal_argument_string());
  }
  if (length == 0) {
    return ReadOnlyRoots(isolate).empty_string();
  }

  if (one_byte) {
    Handle<SeqOneByteString> answer;
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
        isolate, answer, isolate->factory()->NewRawOneByteString(length));
    DisallowGarbageCollection no_gc;
    StringBuilderConcatHelper(*special, answer->GetChars(no_gc),
                              FixedArray::cast(array->elements()),
                              array_length);
    return *answer;
  } else {
    Handle<SeqTwoByteString> answer;
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
        isolate, answer, isolate->factory()->NewRawTwoByteString(length));
    DisallowGarbageCollection no_gc;
    StringBuilderConcatHelper(*special, answer->GetChars(no_gc),
                              FixedArray::cast(array->elements()),
                              array_length);
    return *answer;
  }
}

// Converts a String to JSArray.
// For example, "foo" => ["f", "o", "o"].
RUNTIME_FUNCTION(Runtime_StringToArray) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  Handle<String> s = args.at<String>(0);
  uint32_t limit = NumberToUint32(args[1]);

  s = String::Flatten(isolate, s);
  const int length =
      static_cast<int>(std::min(static_cast<uint32_t>(s->length()), limit));

  Handle<FixedArray> elements = isolate->factory()->NewFixedArray(length);
  bool elements_are_initialized = false;

  if (s->IsFlat() && s->IsOneByteRepresentation()) {
    DisallowGarbageCollection no_gc;
    String::FlatContent content = s->GetFlatContent(no_gc);
    // Use pre-initialized single characters to intialize all the elements.
    // This can be false if the string is sliced from an externalized
    // two-byte string that has only one-byte chars, in that case we will do
    // a LookupSingleCharacterStringFromCode for each of the characters.
    if (content.IsOneByte()) {
      base::Vector<const uint8_t> chars = content.ToOneByteVector();
      FixedArray one_byte_table =
          isolate->heap()->single_character_string_table();
      for (int i = 0; i < length; ++i) {
        Object value = one_byte_table.get(chars[i]);
        DCHECK(value.IsString());
        DCHECK(ReadOnlyHeap::Contains(HeapObject::cast(value)));
        // The single-character strings are in RO space so it should
        // be safe to skip the write barriers.
        elements->set(i, value, SKIP_WRITE_BARRIER);
      }
      elements_are_initialized = true;
    }
  }

  if (!elements_are_initialized) {
    for (int i = 0; i < length; ++i) {
      Handle<Object> str =
          isolate->factory()->LookupSingleCharacterStringFromCode(s->Get(i));
      elements->set(i, *str);
    }
  }

#ifdef DEBUG
  for (int i = 0; i < length; ++i) {
    DCHECK_EQ(String::cast(elements->get(i)).length(), 1);
  }
#endif

  return *isolate->factory()->NewJSArrayWithElements(elements);
}

RUNTIME_FUNCTION(Runtime_StringLessThan) {
  HandleScope handle_scope(isolate);
  DCHECK_EQ(2, args.length());
  Handle<String> x = args.at<String>(0);
  Handle<String> y = args.at<String>(1);
  ComparisonResult result = String::Compare(isolate, x, y);
  DCHECK_NE(result, ComparisonResult::kUndefined);
  return isolate->heap()->ToBoolean(
      ComparisonResultToBool(Operation::kLessThan, result));
}

RUNTIME_FUNCTION(Runtime_StringLessThanOrEqual) {
  HandleScope handle_scope(isolate);
  DCHECK_EQ(2, args.length());
  Handle<String> x = args.at<String>(0);
  Handle<String> y = args.at<String>(1);
  ComparisonResult result = String::Compare(isolate, x, y);
  DCHECK_NE(result, ComparisonResult::kUndefined);
  return isolate->heap()->ToBoolean(
      ComparisonResultToBool(Operation::kLessThanOrEqual, result));
}

RUNTIME_FUNCTION(Runtime_StringGreaterThan) {
  HandleScope handle_scope(isolate);
  DCHECK_EQ(2, args.length());
  Handle<String> x = args.at<String>(0);
  Handle<String> y = args.at<String>(1);
  ComparisonResult result = String::Compare(isolate, x, y);
  DCHECK_NE(result, ComparisonResult::kUndefined);
  return isolate->heap()->ToBoolean(
      ComparisonResultToBool(Operation::kGreaterThan, result));
}

RUNTIME_FUNCTION(Runtime_StringGreaterThanOrEqual) {
  HandleScope handle_scope(isolate);
  DCHECK_EQ(2, args.length());
  Handle<String> x = args.at<String>(0);
  Handle<String> y = args.at<String>(1);
  ComparisonResult result = String::Compare(isolate, x, y);
  DCHECK_NE(result, ComparisonResult::kUndefined);
  return isolate->heap()->ToBoolean(
      ComparisonResultToBool(Operation::kGreaterThanOrEqual, result));
}

RUNTIME_FUNCTION(Runtime_StringEqual) {
  HandleScope handle_scope(isolate);
  DCHECK_EQ(2, args.length());
  Handle<String> x = args.at<String>(0);
  Handle<String> y = args.at<String>(1);
  return isolate->heap()->ToBoolean(String::Equals(isolate, x, y));
}

RUNTIME_FUNCTION(Runtime_FlattenString) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<String> str = args.at<String>(0);
  return *String::Flatten(isolate, str);
}

RUNTIME_FUNCTION(Runtime_StringMaxLength) {
  SealHandleScope shs(isolate);
  return Smi::FromInt(String::kMaxLength);
}

RUNTIME_FUNCTION(Runtime_StringEscapeQuotes) {
  HandleScope handle_scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<String> string = args.at<String>(0);

  // Equivalent to global replacement `string.replace(/"/g, "&quot")`, but this
  // does not modify any global state (e.g. the regexp match info).

  const int string_length = string->length();
  Handle<String> quotes =
      isolate->factory()->LookupSingleCharacterStringFromCode('"');

  int quote_index = String::IndexOf(isolate, string, quotes, 0);

  // No quotes, nothing to do.
  if (quote_index == -1) return *string;

  // Find all quotes.
  std::vector<int> indices = {quote_index};
  while (quote_index + 1 < string_length) {
    quote_index = String::IndexOf(isolate, string, quotes, quote_index + 1);
    if (quote_index == -1) break;
    indices.emplace_back(quote_index);
  }

  // Build the replacement string.
  Handle<String> replacement =
      isolate->factory()->NewStringFromAsciiChecked("&quot;");
  const int estimated_part_count = static_cast<int>(indices.size()) * 2 + 1;
  ReplacementStringBuilder builder(isolate->heap(), string,
                                   estimated_part_count);

  int prev_index = -1;  // Start at -1 to avoid special-casing the first match.
  for (int index : indices) {
    const int slice_start = prev_index + 1;
    const int slice_end = index;
    if (slice_end > slice_start) {
      builder.AddSubjectSlice(slice_start, slice_end);
    }
    builder.AddString(replacement);
    prev_index = index;
  }

  if (prev_index < string_length - 1) {
    builder.AddSubjectSlice(prev_index + 1, string_length);
  }

  return *builder.ToString().ToHandleChecked();
}

RUNTIME_FUNCTION(Runtime_StringIsWellFormed) {
  HandleScope handle_scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<String> string = args.at<String>(0);
  return isolate->heap()->ToBoolean(
      String::IsWellFormedUnicode(isolate, string));
}

RUNTIME_FUNCTION(Runtime_StringToWellFormed) {
  HandleScope handle_scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<String> source = args.at<String>(0);
  if (String::IsWellFormedUnicode(isolate, source)) return *source;
  source = String::Flatten(isolate, source);
  const int length = source->length();
  Handle<SeqTwoByteString> dest =
      isolate->factory()->NewRawTwoByteString(length).ToHandleChecked();
  DisallowGarbageCollection no_gc;
  String::FlatContent source_contents = source->GetFlatContent(no_gc);
  DCHECK(source_contents.IsFlat());
  const uint16_t* source_data = source_contents.ToUC16Vector().begin();
  uint16_t* dest_data = dest->GetChars(no_gc);
  unibrow::Utf16::ReplaceUnpairedSurrogates(source_data, dest_data, length);
  return *dest;
}

}  // namespace internal
}  // namespace v8
