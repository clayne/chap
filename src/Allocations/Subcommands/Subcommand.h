// Copyright (c) 2017 VMware, Inc. All Rights Reserved.
// SPDX-License-Identifier: GPL-2.0

#pragma once
#include <iostream>
#include <memory>
#include <sstream>
#include "../../Commands/Runner.h"
#include "../../Commands/Subcommand.h"
#include "../Finder.h"
#include "../ReferenceConstraint.h"
#include "../SignatureChecker.h"
namespace chap {
namespace Allocations {
namespace Subcommands {
template <class Offset, class Visitor, class Iterator>
class Subcommand : public Commands::Subcommand {
 public:
  typedef typename Finder<Offset>::AllocationIndex AllocationIndex;
  typedef typename Finder<Offset>::Allocation Allocation;
  Subcommand(typename Visitor::Factory& visitorFactory,
             typename Iterator::Factory& iteratorFactory)
      : Commands::Subcommand(visitorFactory.GetCommandName(),
                             iteratorFactory.GetSetName()),
        _visitorFactory(visitorFactory),
        _iteratorFactory(iteratorFactory),
        _processImage(0) {}

  void SetProcessImage(const ProcessImage<Offset>* processImage) {
    _processImage = processImage;
  }

  void Run(Commands::Context& context) {
    Commands::Output& output = context.GetOutput();
    Commands::Error& error = context.GetError();
    bool isRedirected = context.IsRedirected();
    if (_processImage == 0) {
      error << "This command is currently disabled.\n";
      error << "There is no process image.\n";
      if (isRedirected) {
        output << "This command is currently disabled.\n";
        output << "There is no process image.\n";
      }
      return;
    }
    const Finder<Offset>* allocationFinder =
        _processImage->GetAllocationFinder();
    if (allocationFinder == 0) {
      error << "This command is currently disabled.\n";
      error << "Allocations cannot be found.\n";
      if (isRedirected) {
        output << "This command is currently disabled.\n";
        output << "Allocations cannot be found.\n";
      }
      return;
    }
    AllocationIndex numAllocations = allocationFinder->NumAllocations();

    std::unique_ptr<Iterator> iterator;
    iterator.reset(_iteratorFactory.MakeIterator(context, *_processImage,
                                                 *allocationFinder));
    if (iterator.get() == 0) {
      return;
    }

    size_t numPositionals = context.GetNumPositionals();
    size_t nextPositional = 2 + _iteratorFactory.GetNumArguments();

    const SignatureDirectory<Offset>& signatureDirectory =
        _processImage->GetSignatureDirectory();
    const VirtualAddressMap<Offset>& addressMap =
        _processImage->GetVirtualAddressMap();

    std::string signatureString;
    if (nextPositional < numPositionals) {
      size_t signaturePositional = nextPositional++;
      if (nextPositional < numPositionals) {
        error << "Unexpected positional arguments found:\n";
        do {
          error << context.Positional(nextPositional++);
        } while (nextPositional < numPositionals);
        return;
      }
      signatureString = context.Positional(signaturePositional);
    }

    SignatureChecker<Offset> signatureChecker(signatureDirectory, addressMap,
                                              signatureString);
    if (signatureChecker.UnrecognizedSignature()) {
      error << "Signature \"" << signatureString << "\" is not recognized.\n";
      return;
    }

    Offset minSize = 0;
    Offset maxSize = ~((Offset)0);
    bool switchError = false;

    /*
     * It generally does not make sense to specify more than one
     * size argument or more than one minsize argument or more than
     * one maxsize argument but for now this is treated as harmless,
     * just forcing all the constraints to apply.
     */

    size_t numSizeArguments = context.GetNumArguments("size");
    for (size_t i = 0; i < numSizeArguments; i++) {
      Offset size;
      if (!context.ParseArgument("size", i, size)) {
        error << "Invalid size \"" << context.Argument("size", i) << "\"\n";
        switchError = true;
      } else {
        if (minSize < size) {
          minSize = size;
        }
        if (maxSize > size) {
          maxSize = size;
        }
      }
    }
    size_t numMinSizeArguments = context.GetNumArguments("minsize");
    for (size_t i = 0; i < numMinSizeArguments; i++) {
      Offset size;
      if (!context.ParseArgument("minsize", i, size)) {
        error << "Invalid minsize \"" << context.Argument("minsize", i)
              << "\"\n";
        switchError = true;
      } else {
        if (minSize < size) {
          minSize = size;
        }
      }
    }
    size_t numMaxSizeArguments = context.GetNumArguments("maxsize");
    for (size_t i = 0; i < numMaxSizeArguments; i++) {
      Offset size;
      if (!context.ParseArgument("maxsize", i, size)) {
        error << "Invalid maxsize \"" << context.Argument("maxsize", i)
              << "\"\n";
        switchError = true;
      } else {
        if (maxSize > size) {
          maxSize = size;
        }
      }
    }

    std::vector<ReferenceConstraint<Offset> > referenceConstraints;
    const Graph<Offset>* graph = 0;
    size_t numMinIncoming = context.GetNumArguments("minincoming");
    size_t numMaxIncoming = context.GetNumArguments("maxincoming");
    size_t numMinOutgoing = context.GetNumArguments("minoutgoing");
    size_t numMaxOutgoing = context.GetNumArguments("maxoutgoing");
    if (numMinIncoming + numMaxIncoming + numMinOutgoing + numMaxOutgoing > 0) {
      // This is done lazily because it is an expensive calculation.
      graph = _processImage->GetAllocationGraph();
      if (graph == 0) {
        std::cerr
            << "Constraints were placed on incoming or outgoing references\n"
               "but it was not possible to calculate the graph.";
        return;
      }

      switchError =
          switchError ||
          AddReferenceConstraints(
              context, "minincoming", ReferenceConstraint<Offset>::MINIMUM,
              ReferenceConstraint<Offset>::INCOMING, *allocationFinder, *graph,
              signatureDirectory, addressMap, referenceConstraints);
      switchError =
          switchError ||
          AddReferenceConstraints(
              context, "maxincoming", ReferenceConstraint<Offset>::MAXIMUM,
              ReferenceConstraint<Offset>::INCOMING, *allocationFinder, *graph,
              signatureDirectory, addressMap, referenceConstraints);
      switchError =
          switchError ||
          AddReferenceConstraints(
              context, "minoutgoing", ReferenceConstraint<Offset>::MINIMUM,
              ReferenceConstraint<Offset>::OUTGOING, *allocationFinder, *graph,
              signatureDirectory, addressMap, referenceConstraints);
      switchError =
          switchError ||
          AddReferenceConstraints(
              context, "maxoutgoing", ReferenceConstraint<Offset>::MAXIMUM,
              ReferenceConstraint<Offset>::OUTGOING, *allocationFinder, *graph,
              signatureDirectory, addressMap, referenceConstraints);
    }

    if (switchError) {
      return;
    }

    std::unique_ptr<Visitor> visitor;
    visitor.reset(_visitorFactory.MakeVisitor(context, *_processImage));
    if (visitor.get() == 0) {
      return;
    }

    const std::vector<std::string>& taints = _iteratorFactory.GetTaints();
    if (!taints.empty()) {
      error << "The output of this command cannot be trusted:\n";
      if (isRedirected) {
        output << "The output of this command cannot be trusted:\n";
      }
      for (std::vector<std::string>::const_iterator it = taints.begin();
           it != taints.end(); ++it) {
        error << *it;
        if (isRedirected) {
          error << *it;
        }
      }
    }
    for (AllocationIndex index = iterator->Next(); index != numAllocations;
         index = iterator->Next()) {
      const Allocation* allocation = allocationFinder->AllocationAt(index);
      if (allocation == 0) {
        abort();
      }
      Offset size = allocation->Size();

      if (size < minSize || size > maxSize) {
        continue;
      }

      if (!signatureChecker.Check(*allocation)) {
        continue;
      }

      bool unsatisfiedReferenceConstraint = false;
      for (auto constraint : referenceConstraints) {
        if (!constraint.Check(index)) {
          unsatisfiedReferenceConstraint = true;
        }
      }
      if (unsatisfiedReferenceConstraint) {
        continue;
      }

      visitor->Visit(index, *allocation);
    }
  }

  void ShowHelpMessage(Commands::Context& context) {
    Commands::Output& output = context.GetOutput();
    _visitorFactory.ShowHelpMessage(context);
    output << "\n";
    _iteratorFactory.ShowHelpMessage(context);
    output << "\nThe set can be further restricted by appending a class"
              " name or any value\n"
              "in hexadecimal to match against the first "
           << std::dec << ((sizeof(Offset) * 8))
           << "-bit unsigned word, or by specifying\n\"-\" to accept"
              " only unsigned allocations.\n\n"
              "It can also be further restricted by any of the following:\n\n"
              "/minsize <size-in-hex> imposes a minimum size.\n"
              "/maxsize <size-in-hex> imposes a maximum size.\n"
              "/size <size-in-hex> imposes an exact size requirement.\n";
  }

 private:
  typename Visitor::Factory& _visitorFactory;
  typename Iterator::Factory& _iteratorFactory;
  const ProcessImage<Offset>* _processImage;

  bool AddReferenceConstraints(
      Commands::Context& context, const std::string& switchName,
      typename ReferenceConstraint<Offset>::BoundaryType boundaryType,
      typename ReferenceConstraint<Offset>::ReferenceType referenceType,
      const Finder<Offset>& finder, const Graph<Offset>& graph,
      const SignatureDirectory<Offset>& signatureDirectory,
      const VirtualAddressMap<Offset>& addressMap,
      std::vector<ReferenceConstraint<Offset> >& constraints) {
    bool switchError = false;
    size_t numSwitches = context.GetNumArguments(switchName);
    Commands::Error& error = context.GetError();
    for (size_t i = 0; i < numSwitches; ++i) {
      const std::string& signatureAndCount = context.Argument(switchName, i);
      const char* countString = signatureAndCount.c_str();
      // If there is no embedded "=", no signature is wanted and only a count
      // is specified.
      std::string signature;

      size_t equalPos = signatureAndCount.find("=");
      if (equalPos != signatureAndCount.npos) {
        countString += (equalPos + 1);
        signature = signatureAndCount.substr(0, equalPos);
      }
      size_t count = atoi(countString);
      if (count == 0) {
        if (countString[0] != '0' || countString[1] != '\000') {
          error << "Invalid count " << std::dec << "\"" << countString
                << "\".\n";
          switchError = true;
        }
      }
      constraints.emplace_back(signatureDirectory, addressMap,
                                        signature, count, boundaryType,
                                        referenceType, finder, graph);
      if (constraints.back().UnrecognizedSignature()) {
        error << "Signature \"" << signature << "\" is not recognized.\n";
        switchError = true;
      }
    }
    return switchError;
  }
};
}  // namespace Subcommands
}  // namespace Allocations
}  // namespace chap
