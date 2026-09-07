//===- ae.cpp -- Abstract Execution -------------------------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//

#include "AE/Svfexe/AbstractInterpretation.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

#include <string_view>
#include <vector>

using namespace SVF;
using namespace SVFUtil;

int main(int argc, char** argv)
{
    std::vector<char*> arguments(argv, argv + argc);
    arguments.reserve(static_cast<std::size_t>(argc) + 3);
    const auto hasOption = [&](std::string_view option)
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            if (argument == option ||
                    (argument.size() > option.size() &&
                     argument.compare(0, option.size(), option) == 0 &&
                     argument[option.size()] == '='))
                return true;
        }
        return false;
    };
    const auto addDefault = [&](std::string_view option, char* value)
    {
        if (!hasOption(option))
            arguments.push_back(value);
    };
    addDefault("-model-consts", const_cast<char*>("-model-consts=true"));
    addDefault("-model-arrays", const_cast<char*>("-model-arrays=true"));
    addDefault("-pre-field-sensitive",
               const_cast<char*>("-pre-field-sensitive=false"));

    const std::vector<std::string> modules =
        OptionBase::parseOptions(static_cast<int>(arguments.size()),
                                 arguments.data(), "Static Symbolic Execution",
                                 "[options] <input-bitcode...>");

    LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(modules);
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
    AndersenWaveDiff* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    builder.updateCallGraph(ander->getCallGraph());

    AbstractInterpretation& ae = AbstractInterpretation::getAEInstance();
    if (Options::BufferOverflowCheck())
        ae.addDetector(std::make_unique<BufOverflowDetector>());
    if (Options::NullDerefCheck())
        ae.addDetector(std::make_unique<NullptrDerefDetector>());
    ae.runOnModule();

    AndersenWaveDiff::releaseAndersenWaveDiff();
    LLVMModuleSet::releaseLLVMModuleSet();
    return 0;
}
